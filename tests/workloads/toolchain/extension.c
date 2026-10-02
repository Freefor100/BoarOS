#include <lua.h>
#include <lauxlib.h>
static int answer(lua_State *l){lua_pushinteger(l,42);return 1;}
int luaopen_sample(lua_State *l){static const luaL_Reg api[]={{"answer",answer},{0,0}};luaL_newlib(l,api);return 1;}
