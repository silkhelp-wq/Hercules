/**
 * ro-modern direct state channel.
 *
 * Pushes authoritative map-server state straight to the ro-modern gateway over a
 * local UDP socket, instead of the gateway impersonating an RO client.
 *
 * Why this exists: speaking the classic protocol means PACKETVER-shuffled
 * opcodes, per-packetver struct widths and framing quirks that differ between the
 * char and map servers. Every serious bug in the bridge came from that surface -
 * a keepalive opcode that shuffles to 0x0360 in this packetver, a ZC_AID framing
 * difference, silently-dropped walks. Emitting from inside the server sidesteps
 * all of it: the schema below is ours and does not move with PACKETVER.
 *
 * This is additive. It only observes - no hook changes behaviour or return
 * values - so the server plays exactly as it did without the plugin, and a
 * gateway that is not listening costs one unconnected sendto per event.
 *
 * Build:  make plugin.romodern_state
 * Load:   ./map-server --load-plugin romodern_state
 * Tune:   ROMODERN_HOST / ROMODERN_PORT (default 127.0.0.1:7788)
 */

#include "common/hercules.h"
#include "common/memmgr.h"
#include "common/mmo.h"
#include "common/socket.h"
#include "common/strlib.h"
#include "common/timer.h"
#include "map/clif.h"
#include "map/map.h"
#include "map/pc.h"
#include "map/unit.h"
#include "map/mob.h"
#include "map/npc.h"

#include "plugins/HPMHooking.h"
#include "common/HPMDataCheck.h" /* should always be the last Hercules file included! */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

HPExport struct hplugin_info pinfo = {
	"romodern_state",
	SERVER_TYPE_MAP,
	"0.1",
	HPM_VERSION,
};

/* --- wire format ---------------------------------------------------------
 * Deliberately tiny and fixed-width. Little-endian, no alignment padding, no
 * PACKETVER dependence.
 *
 *   u16 magic 'RS'   u8 version   u8 event   ... payload
 */
#define RS_MAGIC   0x5352
#define RS_VERSION 1

enum rs_event {
	RS_MOVE    = 0x01, /* u32 id, i16 x, i16 y, i16 tx, i16 ty          */
	RS_SPAWN   = 0x02, /* u32 id, u8 type, i16 x, i16 y, u16 job,
	                      i32 hp, i32 maxhp, u8 namelen, name           */
	RS_VANISH  = 0x03, /* u32 id                                        */
	RS_DAMAGE  = 0x04, /* u32 src, u32 dst, i32 damage, u8 type         */
};

static int rs_fd = -1;
static struct sockaddr_in rs_addr;

static void rs_open(void)
{
	const char *host = getenv("ROMODERN_HOST");
	const char *port = getenv("ROMODERN_PORT");
	rs_fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (rs_fd < 0) {
		ShowWarning("romodern_state: could not create socket, state channel disabled\n");
		return;
	}
	memset(&rs_addr, 0, sizeof(rs_addr));
	rs_addr.sin_family = AF_INET;
	rs_addr.sin_port = htons((uint16)(port ? atoi(port) : 7788));
	inet_pton(AF_INET, host ? host : "127.0.0.1", &rs_addr.sin_addr);
	ShowStatus("romodern_state: pushing state to %s:%d\n",
		host ? host : "127.0.0.1", port ? atoi(port) : 7788);
}

/* Fire and forget. UDP to a local peer, and a gateway that is not listening
 * simply gets nothing - the server must never block or fail on our account. */
static void rs_send(const uint8 *buf, int len)
{
	if (rs_fd < 0)
		return;
	sendto(rs_fd, buf, (size_t)len, MSG_DONTWAIT,
		(struct sockaddr *)&rs_addr, sizeof(rs_addr));
}

static int rs_head(uint8 *b, uint8 event)
{
	b[0] = RS_MAGIC & 0xFF; b[1] = (RS_MAGIC >> 8) & 0xFF;
	b[2] = RS_VERSION;
	b[3] = event;
	return 4;
}

static int rs_u32(uint8 *b, int o, uint32 v)
{
	b[o] = (uint8)v; b[o+1] = (uint8)(v >> 8);
	b[o+2] = (uint8)(v >> 16); b[o+3] = (uint8)(v >> 24);
	return o + 4;
}

static int rs_i16(uint8 *b, int o, int16 v)
{
	b[o] = (uint8)v; b[o+1] = (uint8)(((uint16)v) >> 8);
	return o + 2;
}

/* --- hooks ---------------------------------------------------------------
 * All post-hooks that return retVal untouched: observation only.
 */

/* A unit was told to walk somewhere. Carries both the current and the target
 * tile so the gateway can start interpolating immediately rather than waiting
 * for the first step. */
static int rs_walk_toxy_post(int retVal, struct block_list *bl, short x, short y, int flag)
{
	if (retVal != 0 || bl == NULL)
		return retVal;
	uint8 b[32];
	int o = rs_head(b, RS_MOVE);
	o = rs_u32(b, o, (uint32)bl->id);
	o = rs_i16(b, o, (int16)bl->x);
	o = rs_i16(b, o, (int16)bl->y);
	o = rs_i16(b, o, x);
	o = rs_i16(b, o, y);
	rs_send(b, o);
	return retVal;
}

/* A unit came into view. clif_set_unit_idle is what builds ZC_NOTIFY_STANDENTRY,
 * so hooking it catches exactly the set of actors a client would be told about. */
static void rs_set_unit_idle_post(struct block_list *bl, struct map_session_data *tsd, enum send_target target)
{
	if (bl == NULL)
		return;
	struct status_data *st = status->get_status_data(bl);
	struct view_data *vd = status->get_viewdata(bl);
	const char *name = clif->get_bl_name(bl);

	uint8 b[96];
	int o = rs_head(b, RS_SPAWN);
	o = rs_u32(b, o, (uint32)bl->id);
	b[o++] = (uint8)clif->bl_type(bl);
	o = rs_i16(b, o, (int16)bl->x);
	o = rs_i16(b, o, (int16)bl->y);
	o = rs_i16(b, o, (int16)(vd ? vd->class : 0));
	o = rs_u32(b, o, (uint32)(st ? st->hp : 0));
	o = rs_u32(b, o, (uint32)(st ? st->max_hp : 0));
	int nl = 0;
	if (name != NULL) {
		nl = (int)strlen(name);
		if (nl > 23) nl = 23;
	}
	b[o++] = (uint8)nl;
	if (nl > 0) { memcpy(b + o, name, (size_t)nl); o += nl; }
	rs_send(b, o);
}

/* A unit left view or died. */
static void rs_clearunit_area_post(struct block_list *bl, enum clr_type type)
{
	if (bl == NULL)
		return;
	uint8 b[16];
	int o = rs_head(b, RS_VANISH);
	o = rs_u32(b, o, (uint32)bl->id);
	b[o++] = (uint8)type;
	rs_send(b, o);
}

/* Damage, so the gateway does not have to infer combat from position changes. */
static int rs_damage_post(int retVal, struct block_list *src, struct block_list *dst,
	int sdelay, int ddelay, int64 in_damage, short div, enum battle_dmg_type type, int64 in_damage2)
{
	if (src == NULL || dst == NULL)
		return retVal;
	uint8 b[24];
	int o = rs_head(b, RS_DAMAGE);
	o = rs_u32(b, o, (uint32)src->id);
	o = rs_u32(b, o, (uint32)dst->id);
	o = rs_u32(b, o, (uint32)in_damage);
	b[o++] = (uint8)type;
	rs_send(b, o);
	return retVal;
}

HPExport void plugin_init(void)
{
	rs_open();
	addHookPost(unit, walk_toxy,        rs_walk_toxy_post);
	addHookPost(clif, set_unit_idle,    rs_set_unit_idle_post);
	addHookPost(clif, clearunit_area,   rs_clearunit_area_post);
	addHookPost(clif, damage,           rs_damage_post);
	ShowStatus("romodern_state: hooks installed (move, spawn, vanish, damage)\n");
}

HPExport void plugin_final(void)
{
	if (rs_fd >= 0) {
		close(rs_fd);
		rs_fd = -1;
	}
	ShowStatus("romodern_state: state channel closed\n");
}
