require "gamelib/game_server.rb"
require 'channellib/channel/channel_system.rb'

class ChannelServer < GameServer

	include ChannelSystem

	def initialize(*args)
		super
		self.init_channel
	end

	# 该节点请求在本地创建一个实例
	def cmd_create_channel(sender, pack)

		channel_id = pack.input.read_string
		channel_klass_name = pack.input.read_string
		channel_remote_klass_name = pack.input.read_string
		channel_data = pack.input.read_hash

		sender.create_channel(eval(channel_remote_klass_name),
		                      channel_klass_name,
		                      self,
		                      channel_data,
		                      channel_id)


	end

	# 该节点请求销毁一个本地通讯通道
	def cmd_destroy_channel(sender, pack)

		channel_id = pack.input.read_string
		sender.destroy_channel(channel_id)

	end


	# 该节点请求调用本地实例的方法
	def cmd_message_channel(sender, pack)
		channel_id = pack.input.read_string
		channel_method = pack.input.read_string
		params_ary = pack.input.read_params_array
		require_return = pack.input.read_byte == 1
		sender.message_channel(channel_id, channel_method, params_ary, require_return ? pack.serial : -1)

	end

  # 本地处理一个包
	# 处理Gate无法与child的channel通讯的问题
  def handle_pack(client, pack)
    method_name = @handle_map_configure[pack.pack_type];
    if (method_name.nil?)
			
			case pack.pack_type
			
			when PACK_TYPE_CREATE_CHANNEL 
				cmd_create_channel(client,pack)
			when PACK_TYPE_DESTROY_CHANNEL 
				cmd_destroy_channel(client,pack)
			when PACK_TYPE_MESSAGE_CHANNEL 
				cmd_message_channel(client,pack)
				
			end
			
		else
			
			super
			
    end

  end


end