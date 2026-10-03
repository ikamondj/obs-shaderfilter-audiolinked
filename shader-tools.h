#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Controller inspector lifecycle runs on OBS's UI thread. */
void sf_tools_init(void);
void sf_tools_shutdown(void);

#ifdef __cplusplus
}
#endif
