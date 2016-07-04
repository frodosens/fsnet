require 'dist_server/server/server_boost'

$server_boost = ServerBoot.new "dist_server/conf.xml"
$server_boost.start("logic", "gate", "center")