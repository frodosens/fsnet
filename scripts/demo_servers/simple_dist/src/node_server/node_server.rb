require 'pack_type.rb'
require 'channel/channel_system.rb'
require 'client/agent_node.rb'
require 'client/tcp_client.rb'
require "channel/channel_server.rb"
require 'node_server/channel/center_channel.rb'
require 'node_server/channel/gate_channel.rb'
require 'node_server/service_channel/login_channel.rb'

class NodeServer < ChannelServer

	include PackTypeDefine

	attr_reader :center_channel

	def initialize(*args)
		super
		self.init_entities
	end

	def on_start_complete
		super
		@center_server   = connect_node("center_server")
		# 注册到中心服务器
		self.reg_to_center
	end


	def reg_to_center


		@center_channel = @center_server.create_channel(NodeServer::CenterChannel, "CenterServer::NodeChannel", self)
		@center_server.send_channel(@center_channel)
		@center_channel.regist_logic_node( self.name ) do |success|

			unless success
				raise " regist logic node fail "
			end

		end


	end

	def cmd_connect(sender, pack)

		# 新连接, 创建新的通讯管道
		login_channel = sender.create_channel( LoginChannel, "LoginChannel", self )
		sender.send_channel(login_channel)
		login_channel.init


		# 告诉中心服务器有人登陆了
		@center_channel.on_login(1)

		# 广播一条登陆消息
		@center_channel.broadcast( :chat, "Hi" )

	end



end