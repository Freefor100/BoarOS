#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
int main(void) {lua_State *l=luaL_newstate();if(!l)return 1;luaL_openlibs(l);int ret=luaL_dostring(l,"assert(6*7==42)");lua_close(l);return ret!=0;}
