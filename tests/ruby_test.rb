require_relative '../ruby/lib/fsnet'

def check(condition, message)
  raise message unless condition
end

server = FSNet::Node.new(token: 'ruby-test-token')
client = FSNet::Node.new(token: 'ruby-test-token')
database = FSNet::Database.new
backend = FSNet::Node.new(token: 'ruby-test-token')
backend.rpc('profile.name') { |request| { 'name' => request.fetch('name').upcase } }
class IdentityService < FSNet::Service
  rpc(:get) { |request| { 'id' => request.fetch('id') } }
end
backend.mount('identity', IdentityService.new)
backend_peer = server.connect(host: '127.0.0.1', port: backend.listen)
server.rpc('player.profile') { |request| server.request(backend_peer, 'profile.name', request) }
server.rpc('player.identity') { |request| server.request(backend_peer, 'identity.get', request) }
server.rpc('player.invalid') { server.request(999999, 'invalid', {}) }
server.rpc('player.save') do |request|
  { 'saved' => database.save(request.fetch('id'), request.fetch('value'), version: request.fetch('version')) }
end
server.rpc('player.get') do |request|
  record = database.get(request.fetch('id'))
  { 'value' => record.value, 'version' => record.version }
end
peer = client.connect(host: '127.0.0.1', port: server.listen)
def invoke(server, client, peer, route, request, extra: nil)
  response = error = nil
  complete = false
  client.call(peer, route, request) { |value, failure| response = value; error = failure; complete = true }
  deadline = Process.clock_gettime(Process::CLOCK_MONOTONIC) + 5
  until complete
    server.poll
    client.poll
    extra&.poll
    GC.start # Exercise callback roots and binary ownership under collection.
    raise 'RPC timed out at test harness' if Process.clock_gettime(Process::CLOCK_MONOTONIC) > deadline
    sleep 0.001
  end
  raise error if error
  response
end
begin
  entities = FSNet::EntityStore.new(database)
  check(entities.transfer('entity42', 'logic-a'), 'initial ownership failed')
  fence = entities.owner('entity42').version
  check(entities.save('entity42', {coins: 10}, fence: fence), 'fenced save failed')
  state = entities.load('entity42')
  check(entities.transfer('entity42', 'logic-b', expected_fence: fence), 'takeover failed')
  check(!entities.save('entity42', {coins: 0}, fence: fence, version: state.version), 'stale owner wrote data')
  check(invoke(server, client, peer, 'player.save', {id: '42', value: {name: '测试', coins: 12}, version: 0})['saved'], 'create failed')
  check(!invoke(server, client, peer, 'player.save', {id: '42', value: {}, version: 0})['saved'], 'stale write accepted')
  record = invoke(server, client, peer, 'player.get', {id: '42'})
  check(record['value'] == {'name' => '测试', 'coins' => 12} && record['version'] > 0, 'round trip mismatch')
  check(invoke(server, client, peer, 'player.profile', {name: 'alice'}, extra: backend) == {'name' => 'ALICE'}, 'fiber RPC failed')
  check(invoke(server, client, peer, 'player.identity', {id: '42'}, extra: backend) == {'id' => '42'}, 'service mount failed')
  begin
    invoke(server, client, peer, 'instance_eval', {})
    raise 'unregistered method executed'
  rescue FSNet::RPCError => error
    check(error.status == 1, 'wrong missing route status')
  end
  puts 'PASS Ruby 3: real RPC, JSON, storage CAS, GC roots, explicit RPC registration'
ensure
  client.close
  server.close
  database.close
  backend.close
end
