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
 * Build:  make plugin.eoe_state
 * Load:   ./map-server --load-plugin eoe_state
 * Tune:   EOE_HOST / EOE_PORT (default 127.0.0.1:7788)
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
	"eoe_state",
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
#define ES_MAGIC   0x5345
#define ES_VERSION 1

enum es_event {
	ES_MOVE    = 0x01, /* u32 id, i16 x, i16 y, i16 tx, i16 ty          */
	ES_SPAWN   = 0x02, /* u32 id, u8 type, i16 x, i16 y, u16 job,
	                      i32 hp, i32 maxhp, u8 namelen, name           */
	ES_VANISH  = 0x03, /* u32 id                                        */
	ES_DAMAGE  = 0x04, /* u32 src, u32 dst, i32 damage, u8 type         */
};

static int es_fd = -1;
static struct sockaddr_in es_addr;

static void es_open(void)
{
	const char *host = getenv("EOE_HOST");
	const char *port = getenv("EOE_PORT");
	es_fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (es_fd < 0) {
		ShowWarning("eoe_state: could not create socket, state channel disabled\n");
		return;
	}
	memset(&es_addr, 0, sizeof(es_addr));
	es_addr.sin_family = AF_INET;
	es_addr.sin_port = htons((uint16)(port ? atoi(port) : 7788));
	inet_pton(AF_INET, host ? host : "127.0.0.1", &es_addr.sin_addr);
	ShowStatus("eoe_state: pushing state to %s:%d\n",
		host ? host : "127.0.0.1", port ? atoi(port) : 7788);
}

/* Fire and forget. UDP to a local peer, and a gateway that is not listening
 * simply gets nothing - the server must never block or fail on our account. */
static void es_send(const uint8 *buf, int len)
{
	if (es_fd < 0)
		return;
	sendto(es_fd, buf, (size_t)len, MSG_DONTWAIT,
		(struct sockaddr *)&es_addr, sizeof(es_addr));
}

static int es_head(uint8 *b, uint8 event)
{
	b[0] = ES_MAGIC & 0xFF; b[1] = (ES_MAGIC >> 8) & 0xFF;
	b[2] = ES_VERSION;
	b[3] = event;
	return 4;
}

static int es_u32(uint8 *b, int o, uint32 v)
{
	b[o] = (uint8)v; b[o+1] = (uint8)(v >> 8);
	b[o+2] = (uint8)(v >> 16); b[o+3] = (uint8)(v >> 24);
	return o + 4;
}

static int es_i16(uint8 *b, int o, int16 v)
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
static int es_walk_toxy_post(int retVal, struct block_list *bl, short x, short y, int flag)
{
	if (retVal != 0 || bl == NULL)
		return retVal;
	uint8 b[32];
	int o = es_head(b, ES_MOVE);
	o = es_u32(b, o, (uint32)bl->id);
	o = es_i16(b, o, (int16)bl->x);
	o = es_i16(b, o, (int16)bl->y);
	o = es_i16(b, o, x);
	o = es_i16(b, o, y);
	es_send(b, o);
	return retVal;
}

/* A unit came into view.
 *
 * There are TWO of these and hooking only one is a bug that hides itself: a unit
 * standing still is announced by clif_set_unit_idle (ZC_NOTIFY_STANDENTRY) and a
 * unit that is walking by clif_set_unit_walking (ZC_NOTIFY_MOVEENTRY). Wandering
 * monsters are usually walking, so with only the idle hook the gateway saw
 * whichever few happened to be standing still at the moment you looked - which
 * reads as "the state channel works, the map is just quiet". */
static void es_spawn(struct block_list *bl)
{
	if (bl == NULL)
		return;
	struct status_data *st = status->get_status_data(bl);
	struct view_data *vd = status->get_viewdata(bl);
	const char *name = clif->get_bl_name(bl);

	uint8 b[96];
	int o = es_head(b, ES_SPAWN);
	o = es_u32(b, o, (uint32)bl->id);
	b[o++] = (uint8)clif->bl_type(bl);
	o = es_i16(b, o, (int16)bl->x);
	o = es_i16(b, o, (int16)bl->y);
	o = es_i16(b, o, (int16)(vd ? vd->class : 0));
	o = es_u32(b, o, (uint32)(st ? st->hp : 0));
	o = es_u32(b, o, (uint32)(st ? st->max_hp : 0));
	int nl = 0;
	if (name != NULL) {
		nl = (int)strlen(name);
		if (nl > 23) nl = 23;
	}
	b[o++] = (uint8)nl;
	if (nl > 0) { memcpy(b + o, name, (size_t)nl); o += nl; }
	es_send(b, o);
}

static void es_set_unit_idle_post(struct block_list *bl, struct map_session_data *tsd,
	enum send_target target)
{
	es_spawn(bl);
}

static void es_set_unit_walking_post(struct block_list *bl, struct map_session_data *tsd,
	struct unit_data *ud, enum send_target target)
{
	es_spawn(bl);
}

/* A unit left view or died. */
static void es_clearunit_area_post(struct block_list *bl, enum clr_type type)
{
	if (bl == NULL)
		return;
	uint8 b[16];
	int o = es_head(b, ES_VANISH);
	o = es_u32(b, o, (uint32)bl->id);
	b[o++] = (uint8)type;
	es_send(b, o);
}

/* Damage, so the gateway does not have to infer combat from position changes. */
static int es_damage_post(int retVal, struct block_list *src, struct block_list *dst,
	int sdelay, int ddelay, int64 in_damage, short div, enum battle_dmg_type type, int64 in_damage2)
{
	if (src == NULL || dst == NULL)
		return retVal;
	uint8 b[24];
	int o = es_head(b, ES_DAMAGE);
	o = es_u32(b, o, (uint32)src->id);
	o = es_u32(b, o, (uint32)dst->id);
	o = es_u32(b, o, (uint32)in_damage);
	b[o++] = (uint8)type;
	es_send(b, o);
	return retVal;
}

HPExport void plugin_init(void)
{
	es_open();
	addHookPost(unit, walk_toxy,        es_walk_toxy_post);
	addHookPost(clif, set_unit_idle,    es_set_unit_idle_post);
	addHookPost(clif, set_unit_walking, es_set_unit_walking_post);
	addHookPost(clif, clearunit_area,   es_clearunit_area_post);
	addHookPost(clif, damage,           es_damage_post);
	ShowStatus("eoe_state: hooks installed (move, spawn idle+walking, vanish, damage)\n");
}

HPExport void plugin_final(void)
{
	if (es_fd >= 0) {
		close(es_fd);
		es_fd = -1;
	}
	ShowStatus("eoe_state: state channel closed\n");
}
