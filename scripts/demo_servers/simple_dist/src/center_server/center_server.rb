
require 'pack_type.rb'
require "game_server.rb"
require 'util/uuid.rb'
require 'util/hash.rb'
require 'center_server/entity/center_entity.rb'


class CenterServer < GameServer

	attr_reader :nodes
	def on_start_complete
		super

		@nodes = {}

	end


	# 请求注册为一个逻辑节点
	def cmd_regist_node(sender, pack)

		node_name = pack.input.read_string

		@nodes[node_name] = sender

		info("cmd_regist_node #{name} : #{sender}")

		# 纯粹的回应成功
		sender.send_pack(pack)

	end
	# 请求广播到所有的逻辑节点
	def cmd_broadcast(sender, pack)

	end

	# 请求调用指定一个逻辑节点的函数
	def cmd_call_node_rpc(sender, pack)
		# 调用那个一个节点
		node_name = pack.input.read_string
		if @nodes[node_name].nil?
			err("node is not exist")
			return;
		end

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