-- Structure/container types stay in shared Lua, not in native game classes.
local Item=fsnet.struct('SampleItem',{id='string',count='int32'})
local Task=fsnet.struct('SampleTask',{progress='int32',done='bool'})
local Pose=fsnet.struct('SamplePose',{origin='vec3',label='string'})
Profile=fsnet.Entity:extend('Profile')
Profile:attr('position',{type='vec3',client='rw',sync='owner'})
Profile:attr('transform',{type='mat4x4',client='rw',sync='owner'})
Profile:attr('pose',{type=Pose,client='rw',sync='owner'})
Profile:attr('items',{type=fsnet.list(Item,64),sync='owner'})
Profile:attr('tasks',{type=fsnet.map(Task,64),sync='owner'})
Profile:attr('tags',{type=fsnet.set('string',32),sync='owner'})
function Profile:validate_write(values)
    return not values.position or math.abs(values.position.x)<=100
end
-- Fixture action demonstrating server-origin container changes; a real reward
-- service would validate its own conditions before granting items.
Profile:method('sample',{runs_on='server'})
Profile:rpc('sample',{callers={'client'},access='owner'})
function Profile:sample(step)
    if step=='add' then
        assert(#self.items==0,'already added')
        self.items:add({id='potion',count=1})
        self.tasks['daily.1']={progress=0,done=false};self.tags:add('questing')
    elseif step=='finish' then
        self.items[1].count=2;self.tasks['daily.1'].progress=10;self.tasks['daily.1'].done=true
    elseif step=='remove' then
        self.items:remove(1);self.tasks['daily.1']=nil;self.tags:remove('questing')
    else self:reject() end
end
Profile:define()
