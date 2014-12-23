require "game_server.rb"
require 'pack_type.rb'
require 'util/uuid.rb'
require 'util/hash.rb'
require 'entity/entity_system.rb'
require 'client/agent_node.rb'
require 'client/tcp_client.rb'
require 'node_server/entity/center_entity.rb'

class NodeServer < GameServer

	include PackTypeDefine
	include EntitySystem

	def initialize(*args)
		super
		self.init_entities
	end

	def on_start_complete
		super
		@center_server   = connect_node("center_server")
		self.create_server_entity
		self.reg_to_center
	end

	def create_server_entity
		self.create_entity(NodeServer::CenterEntity, "NodeServer::CenterEntity")
	end

	def reg_to_center

		os = FSOutputStream.new
		os.write_string(self.name)
		pack = Pack.create(Pack.generate_serial,PACK_TYPE_REGIST_NODE, os)

		@center_server.send_pack(pack, nil, proc{
			info("reg_to_center is successful")

			entity = @center_server.create_entity(NodeServer::CenterEntity, "CenterServer::CenterEntity")
			@center_server.send_entity(entity)
			# RPC call
			entity.hello("Hey") do |ret|
				p ret
			end

		})

	end

	# 请求连接
	def cmd_connect(sender, pack)



	end



	# 该节点请求在本地创建一个实例
	def cmd_create_entity(sender, pack)

		entity_id = pack.input.read_string
		entity_klass_name = pack.input.read_string
		entity_remote_klass_name = pack.input.read_string
		entity_data = pack.input.read_hash

		sender.create_entity(eval(entity_remote_klass_name), eval(entity_klass_name), entity_data, entity_id)


	end

	# 该节点请求销毁一个本地实例
	def cmd_destroy_entity(sender, pack)

		entity_id = pack.input.read_string
		sender.destroy_entity(entity_id)

	end

	# 该节点请求调用本地实例的方法
	def cmd_message_entity(sender, pack)

		entity_id = pack.input.read_string
		entity_method = pack.input.read_string
		params_ary = pack.input.read_params_array
		require_return = pack.input.read_byte == 1
		sender.message_entity(entity_id, entity_method, params_ary, require_return ? pack.serial : -1)

	end


end