require_relative '../ruby/lib/fsnet'

database = FSNet::Database.new(path: ENV.fetch('FSNET_DATABASE', 'players.db'))
node = FSNet::Node.new(token: ENV.fetch('FSNET_CLUSTER_TOKEN'))
node.rpc('player.get') do |request|
  record = database.get("player:#{request.fetch('id')}")
  record && { 'player' => record.value, 'version' => record.version }
end
node.rpc('player.save') do |request|
  { 'saved' => database.save("player:#{request.fetch('id')}", request.fetch('player'),
                            version: request.fetch('version', 0)) }
end
puts "player service listening #{node.listen(port: Integer(ENV.fetch('FSNET_PORT', '7001')))}"
begin
  node.run
ensure
  node.close
  database.close
end
