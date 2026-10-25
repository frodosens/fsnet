-- Shared declarations. Items/tasks are Lua business types, not native classes.
local Item=fsnet.struct('Item',{id='string',count='int32'})
local Task=fsnet.struct('Task',{progress='int32',done='bool'})
ContainerPlayer=fsnet.Entity:extend('ContainerDemo',{owner='legacy_id'})
ContainerPlayer:attr('items',{type=fsnet.list(Item,64),sync='owner'})
ContainerPlayer:attr('tasks',{type=fsnet.map(Task,64),sync='owner'})
ContainerPlayer:attr('tags',{type=fsnet.set('string',32),sync='owner'})
ContainerPlayer:attr('granted',{type='bool',default=false,client='none',sync='none'})

-- Only trusted server calls may grant/remove items. Clients read replicas.
ContainerPlayer:rpc('grant_demo',{callers={'server'}})
function ContainerPlayer:grant_demo()
    if self.granted then return end
    self.items:add({id='potion',count=1})
    self.tasks['daily.1']={progress=0,done=false}
    self.tags:add('questing')
    self.granted=true
end
ContainerPlayer:rpc('finish_demo',{callers={'server'}})
function ContainerPlayer:finish_demo()
    assert(#self.items>0 and self.tasks['daily.1'],'grant first')
    self.items[1].count=2
    self.tasks['daily.1'].progress=10
    self.tasks['daily.1'].done=true
end
ContainerPlayer:rpc('remove_demo',{callers={'server'}})
function ContainerPlayer:remove_demo()
    if #self.items>0 then self.items:remove(1) end
    self.tasks['daily.1']=nil
    self.tags:remove('questing')
end
-- Fixture RPC only. Production gameplay decides when to call grant/remove.
ContainerPlayer:rpc('demo',{callers={'client'},access='owner'})
function ContainerPlayer:demo(step)
    if step=='grant' then self:grant_demo()
    elseif step=='finish' then self:finish_demo()
    elseif step=='remove' then self:remove_demo()
    else error('unknown demo step') end
end
ContainerPlayer:define()
