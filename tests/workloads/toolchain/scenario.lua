local f=assert(io.open('lua-data','w'));f:write('native Lua\n');f:close()
f=assert(io.open('lua-data'));assert(f:read('*a')=='native Lua\n');f:close()
package.path='./?.lua;'..package.path
f=assert(io.open('module.lua','w'));f:write('return { value = 42 }\n');f:close()
assert(require('module').value==42)
local p=assert(io.popen('printf child-result'));assert(p:read('*a')=='child-result');assert(p:close())
assert(os.execute('true'));local ok,reason,status=os.execute('false');assert(ok==nil and reason=='exit' and status==1)
print('Lua native scenario passed')
