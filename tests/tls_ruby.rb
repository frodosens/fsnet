require_relative '../ruby/lib/fsnet'
root = ARGV.fetch(0)
security = ->(name) { {ca: "#{root}/ca.pem", certificate: "#{root}/#{name}.pem", key: "#{root}/#{name}.key", peer_name: 'server.fsnet'} }
identities = []
server = FSNet::Node.new(token: 'token', tls: security.call('server'), authorize: ->(_peer, identity, route) {
  identities << identity
  route == 'echo'
})
client = FSNet::Node.new(token: 'token', tls: security.call('client'))
begin
  port = server.listen
  server.rpc_bytes('echo') { |data| data }
  peer = client.connect(host: '127.0.0.1', port: port)
  completed = nil
  client.call_bytes(peer, 'echo', "binary\0data".b) { |data, error| completed = [data, error] }
  deadline = Process.clock_gettime(Process::CLOCK_MONOTONIC) + 3
  until completed
    server.poll; client.poll
    raise 'TLS deadline' if Process.clock_gettime(Process::CLOCK_MONOTONIC) >= deadline
    sleep 0.001
  end
  raise 'TLS bytes mismatch' unless completed == ["binary\0data".b, nil]
  raise 'missing authenticated identity' unless identities.all? { |id| id.match?(/\Asha256:[0-9a-f]{64}\z/) } && !identities.empty?
  puts 'PASS Ruby TLS adapter: verified certificates, principal callback and binary RPC'
ensure
  client.close; server.close
end
