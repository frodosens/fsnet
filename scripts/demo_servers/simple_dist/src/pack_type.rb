

module PackTypeDefine

	# 协议type枚举定义  #对应到configure里的pack_handle 的 key, 全局唯一
	PACK_TYPE_CONNECT = 1
	PACK_TYPE_CREATE_ENTITY = 2
	PACK_TYPE_DESTROY_ENTITY = 3
	PACK_TYPE_MESSAGE_ENTITY = 4
	PACK_TYPE_MESSAGE_RETURN = 5
	# 节点与中心管理器的协议
	PACK_TYPE_REGIST_NODE = 6
	PACK_TYPE_CALL_GLOBAL_RPC = 7
	PACK_TYPE_CALL_NODE_RPC = 8


end

class GameServer

	include PackTypeDefine

end