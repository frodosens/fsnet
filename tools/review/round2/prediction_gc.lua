local C=fsnet.Entity:extend('Memory')
C:attr('value',{type=fsnet.array(fsnet.array('int32',256),15),authority='driver'})
function C:step() return '' end
C:define()
runtime.start(function()
 local confirmed=C:legacy_watch('alice')
 local prediction=confirmed:prediction()
 for i=1,16 do
  local ok,err=pcall(prediction.push,prediction,prediction.pending+1,'step','')
  print(i,ok,err)
 end
 print('pending',prediction.pending)
end)
