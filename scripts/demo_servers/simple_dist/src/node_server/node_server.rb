require 'pack_type.rb'
require 'channel/channel_system.rb'
require 'client/agent_node.rb'
require 'client/tcp_client.rb'
require "channel/channel_server.rb"
require 'node_server/channel/center_channel.rb'
require 'node_server/service_channel/login_channel.rb'

class NodeServer < ChannelServer

	include PackTypeDefine

	def initialize(*args)
		super
		self.init_entities
	end

	def on_start_complete
		super
		@center_server   = connect_node("center_server")
		self.create_server_channel
		self.reg_to_center
	end

	def create_server_channel
		self.create_channel(LoginChannel, "NodeServer::LoginChannel")
	end

	def reg_to_center

		os = FSOutputStream.new
		os.write_string(self.name)
		pack = Pack.create(Pack.generate_serial,PACK_TYPE_REGIST_NODE, os)

		@center_server.send_pack(pack, nil, proc{
			info("reg_to_center is successful")

			channel = @center_server.create_channel(NodeServer::CenterChannel, "CenterServer::CenterChannel")
			@center_server.send_channel(channel)
			channel.get_online_count do |ret|
				p ret
			end
		})

	end

	# 请求广播
	def cmd_broadcast(sender, pack)

		for client_id, client in @clients
			client.send_pack(pack)
		end

	end

	# 请求连接
	def cmd_connect(sender, pack)


	  os = FSOutputStream.new
	  os.write_string("Hey")
		pack = Pack.create(0, PACK_TYPE_CHAT_MESSAGE, os)

		channel = @center_server.find_channel(NodeServer::CenterChannel)
		channel.broadcast(pack)

	end


end