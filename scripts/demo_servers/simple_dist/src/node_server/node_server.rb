require 'channellib/channel_boost.rb'

# 对外的通讯渠道
require 'node_server/channel/center_channel.rb'
require 'node_server/channel/gate_channel.rb'
require 'node_server/channel/database_channel.rb'

# 对客户端的通讯渠道
require 'node_server/service_channel/login_channel.rb'

class NodeServer < ChannelServer

	include PackTypeDefine

	attr_reader :center_channel
	attr_reader :database_channel

	def initialize(*args)
		super
		self.init_channel
	end

	def on_start_complete
		super
		@center_server   = connect_node("center_server")
		@database_server   = connect_node("database_server")
		# 从数据库初始化
		self.init_database
	end

	def init_database

		@database_channel = @database_server.create_channel(NodeServer::DatabaseChannel, "DatabaseServer::OwnerChannel", self)
		@database_server.send_channel(@database_channel)

		@database_channel.find_table_by( "server_init_table", "node_init_conf" ) do |ret|

			# init from ret
			info( "init from #{ret}" )

			# 注册到中心服务器
			self.init_center

		end

	end


	def init_center


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

		# 登陆的PID
		pid = 0

		# 告诉中心服务器有人登陆了
		@center_channel.on_login(self.name, pid)

		# 广播一条登陆消息
		@center_channel.broadcast( :chat, "Hi" )

		# 通过PID找到channel
		@center_channel.find_channel_by_pid(pid) do |channel_name|
				@center_channel.call_channel( channel_name, :chat, "Yo" )
		end


	end



end