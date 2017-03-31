#ifndef SC_UPDATER_H
#define SC_UPDATER_H

#include "systemc.h"
#include <trest.h>

#define DEVICE_TRAIL_ENDPOINT_FMT "/trails/%s/steps"
#define DEVICE_STEP_ENDPOINT_FMT "/trails/%s/steps/%d/progress"
#define DEVICE_STEP_STATUS_FMT "{ \"status\" : \"%s\", \"status-msg\" : \"%s\", \"progress\" : %d }"

#define TRAIL_OBJECT_DL_FMT	"/objects/%s"

struct trail_remote {
	trest_ptr client;
	char *endpoint;
	struct sc_state *pending;
};

int sc_trail_update_start(struct systemc *sc, int offline);
int sc_trail_update_finish(struct systemc *sc);
int sc_trail_update_install(struct systemc *sc);
int sc_trail_check_for_updates(struct systemc *sc);
int sc_trail_do_single_update(struct systemc *sc);
void sc_trail_remote_destroy(struct systemc *sc);

void sc_bl_set_current(struct systemc *sc, int rev);
int sc_bl_get_update(struct systemc *sc, int *update);
int sc_bl_clear_update(struct systemc *sc);

#endif
