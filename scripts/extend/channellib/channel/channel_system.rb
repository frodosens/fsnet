
require 'fiber'

module ChannelSystem

	attr_reader :entities

	def init_channel
		@channels = {}
	end

	def find_channel(key, operation={ :with => :klass })
		if operation[:with] == :uuid
			return @channels[key]
		end


		if operation[:with] == :klass
			for k, v in @channels
				if v.class == key
					return v
				end
			end
		end
		nil
	end

	def create_channel(klass, remote_klass_name=klass.to_s, local_owner=nil, channel_data={}, channel_id=nil)

		channel = klass.create(channel_id)
		channel.remote_klass_name = remote_klass_name
		channel.owner = self
		channel.local_owner = local_owner
		channel.on_create_data = channel_data
		channel.on_create(channel_data)
		@channels[channel.uuid] = channel

	end

	def destroy_channel(uuid)

		channel = @channels[uuid]
		if channel.nil?
			self.server.warn("try destroy channel fail : #{uuid}")
			return
		end

		channel.on_destroy
		@channels.delete(uuid)

	end

	def channel_missing( uuid, method_name, params, call_serial )
		self.server.warn("#{self.class} try call message #{method_name} channel fail : #{uuid}")
	end

	def message_channel( uuid, method_name, params, call_serial=0 )


		channel = @channels[uuid]
		if channel.nil?
			return channel_missing(uuid, method_name, params, call_serial)
		end
		channel.calling_serial = call_serial

		channel.send(method_name, *params)
		
		channel.calling_serial = -1
	end




end