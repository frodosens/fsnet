require_relative '../../ruby/lib/fsnet'
$stdout.sync = true
node = FSNet::Node.new(token: ENV.fetch('FSNET_CLUSTER_TOKEN'))
peer = node.connect(host: '127.0.0.1', port: Integer(ARGV.fetch(0)))
node.rpc_bytes('contract.echo') { |bytes| bytes }
node.rpc_bytes('contract.forward') { |bytes| node.request_bytes(peer, 'backend.echo', bytes) }
node.rpc_bytes('contract.fail') { raise 'intentional contract error' }
node.rpc_bytes('contract.timeout') do |bytes|
  begin
    node.request_bytes(peer, 'backend.hang', bytes, timeout: 30)
    raise 'expected a timeout'
  rescue FSNet::RPCError => error
    raise unless error.status == 3
    'timeout'
  end
end
puts "listening #{node.listen}"
begin
  node.run
ensure
  node.close
end
