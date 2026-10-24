-- Shared typed declarations; no gameplay types are compiled into C++.
local Vec3=fsnet.struct('Vec3',{x='number',y='number',z='number'})
local Pose=fsnet.struct('Pose',{position=Vec3,rotation=Vec3})
StructuredEntity=fsnet.Entity:extend('StructuredDemo',{owner='id'})
StructuredEntity:attr('position',{
    type=Vec3, default={x=0,y=0,z=0},
    authority='driver', client='rw', sync='observers',
})
StructuredEntity:attr('matrix',{
    type='mat4x4', client='rw', sync='observers',
}) -- Built-in 4x4 number array; zero default.
StructuredEntity:attr('pose',{
    type=Pose, client='rw', sync='observers',
})
function StructuredEntity:validate_write(proposal)
    return not proposal.position or math.abs(proposal.position.x)<=100
end
StructuredEntity:define()
