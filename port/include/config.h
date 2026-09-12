#pragma once

#include <PR/ultratypes.h>

#define CONFIG_FNAME "pd.ini"
#define CONFIG_PATH "$S/" CONFIG_FNAME

void configInit(void);

// loads config from file (path extensions such as ! apply)
s32 configLoad(const char *fname);

// saves config to file (path extensions such as ! apply)
s32 configSave(const char *fname);

// One section's live settings as "key=value" lines. See the definition.
u32 configDumpSection(const char *section, char *dst, u32 dstsize);

// registers a variable in the config file
// this should be done before configInit() is called, preferably in a module constructor
void configRegisterInt(const char *key, s32 *var, s32 min, s32 max);
void configRegisterUInt(const char* key, u32* var, u32 min, u32 max);
void configRegisterFloat(const char *key, f32 *var, f32 min, f32 max);
void configRegisterString(const char *key, char *var, u32 maxstr);
