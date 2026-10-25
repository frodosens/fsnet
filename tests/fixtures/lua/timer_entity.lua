TimerActor=fsnet.Entity:extend('TimerActor')
TimerActor:attr('count',{type='int64',default=0})
local fail_once=true
function TimerActor:add(payload,info)
    assert(info.fired_at>=info.scheduled_at and info.late_ms>=0)
    self.count=self.count+assert(tonumber(payload))
end
function TimerActor:flaky(payload,info)
    if fail_once then fail_once=false;self.count=self.count+1000;error('injected timer failure') end
    return self:add(payload,info)
end
function TimerActor:cancel_self(payload,info)
    self:add(payload,info);assert(self:cancel_timer(info.id))
end
function TimerActor:renew(payload,info)
    self:add(payload,info);self:after(100,'add','2',{key=info.id})
end
TimerActor:rpc('arm',{callers={'server'}})
function TimerActor:arm(mode)
    if mode=='once' then return self:after(100,'add','3',{key='once'})
    elseif mode=='period' then return self:every(100,'add','1',{key='period',count=3})
    elseif mode=='catchup' then return self:every(100,'add','1',{key='repeat',count=4,missed='catch_up'})
    elseif mode=='coalesce' then return self:every(100,'add','1',{key='repeat',count=4})
    elseif mode=='delay' then return self:every(100,'add','1',{key='delay',count=2,mode='fixed_delay'})
    elseif mode=='cancel' then
        local id=self:after(100,'add','100',{key='cancel'})
        assert(self:timer(id).method=='add');assert(self:cancel_timer(id));assert(not self:cancel_timer(id))
    elseif mode=='old' then return self:after(100,'add','100',{key='replace'})
    elseif mode=='replace' then return self:after(200,'add','2',{key='replace'})
    elseif mode=='fail' then return self:after(100,'flaky','5',{key='flaky'})
    elseif mode=='rollback' then self:after(100,'add','100',{key='rollback'});error('rollback timer registration')
    elseif mode=='self_cancel' then return self:every(100,'cancel_self','1',{key='self-cancel'})
    elseif mode=='renew' then return self:after(100,'renew','1',{key='renew'})
    elseif mode=='quota' then
        for i=1,64 do self:after(1000000,'add','1',{key='quota-'..i}) end
        assert(not pcall(function() self:after(1000000,'add','100',{key='overflow'}) end))
    else error('unknown timer fixture mode') end
end
TimerActor:register()
