-- Only method names and bytes are persisted, never Lua closures or stacks.
local TimerDemo=fsnet.Entity:extend('TimerDemo')
TimerDemo:attr('initialized',{type='bool',default=false})
TimerDemo:attr('delayed',{type='int32',default=0})
TimerDemo:attr('periodic',{type='int32',default=0})
TimerDemo:attr('caught_up',{type='int32',default=0})
TimerDemo:attr('absolute',{type='int32',default=0})
TimerDemo:attr('cancelled',{type='int32',default=0})
TimerDemo:attr('finished_job',{type='string',default='pending'})

-- These ordinary server methods are not exposed as client RPCs.
function TimerDemo:finish_job(job_id,timer)
    assert(timer.id=='job')
    self.delayed=self.delayed+1
    self.finished_job=job_id
end
function TimerDemo:periodic_tick() self.periodic=self.periodic+1 end
function TimerDemo:catch_up_tick() self.caught_up=self.caught_up+1 end
function TimerDemo:alarm() self.absolute=self.absolute+1 end
function TimerDemo:should_never_run() self.cancelled=self.cancelled+1 end

TimerDemo:rpc('initialize',{callers={'server'}})
function TimerDemo:initialize()
    -- This flag and all registrations commit in the same entity transaction.
    -- Loading the script again must not reset existing tasks or repeat counts.
    if self.initialized then return end
    local id=self:after(2500,'finish_job','job-42',{key='job'})
    local job=self:timer(id)
    assert(job.method=='finish_job')
    -- An absolute UTC deadline. Reuse a queried deadline so the example does
    -- not need a separate wall-clock API in the script sandbox.
    self:at(job.scheduled_at+250,'alarm','',{key='alarm'})
    self:every(500,'periodic_tick','',{
        key='periodic',count=3,delay_ms=2500,missed='coalesce',
    })
    self:every(500,'catch_up_tick','',{
        key='catch-up',count=3,delay_ms=2500,missed='catch_up',
    })
    local cancelled=self:after(2500,'should_never_run','',{key='cancelled'})
    assert(self:cancel_timer(cancelled))
    assert(self:timer(cancelled)==nil)
    self.initialized=true
end

-- A read-only, public status endpoint for this loopback demonstration.
TimerDemo:rpc('status',{callers={'client'}})
function TimerDemo:status()
    return table.concat({self.delayed,self.periodic,self.caught_up,
        self.absolute,self.cancelled,self.finished_job},':')
end
TimerDemo:register()

runtime.start(function()
    -- The same key and initial values recover the same durable global ID.
    local demo=TimerDemo:spawn({},{
        creation_key='timer-demo',bootstrap_scope='examples',
        target_node='local',persistence='durable',
    })
    -- An abruptly killed owner can retain its lease briefly. Retry this
    -- idempotent bootstrap operation rather than bypassing its fence.
    local initialized=false
    for attempt=1,100 do
        if pcall(function() demo:initialize('') end) then initialized=true;break end
        runtime.sleep(20)
    end
    assert(initialized,'timer demo bootstrap timed out')
    print('timers ready '..demo.id)
end)
