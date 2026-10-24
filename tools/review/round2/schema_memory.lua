for i=1,64 do
 local C=fsnet.Entity:extend('Memory'..i)
 C:attr('value',{type=fsnet.array(fsnet.array('int32',256),15)})
 C:define()
end
