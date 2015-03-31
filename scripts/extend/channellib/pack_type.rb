
require 'gamelib/game_server.rb'

module PackTypeDefine

	# 协议type枚举定义  #对应到configure里的pack_handle 的 key, 全局唯一
	PACK_TYPE_CONNECT = 1
	PACK_TYPE_CREATE_CHANNEL = 2
	PACK_TYPE_DESTROY_CHANNEL = 3
	PACK_TYPE_MESSAGE_CHANNEL = 4
	PACK_TYPE_MESSAGE_RETURN = 5
	PACK_TYPE_RECONNECT = 6


end

class GameServer < GameTCPServer

	include PackTypeDefine

end