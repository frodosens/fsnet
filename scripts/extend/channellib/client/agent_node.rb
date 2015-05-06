

require 'channellib/pack_type.rb'
require 'channellib/util/uuid.rb'
require 'channellib/util/hash.rb'
require 'channellib/channel/channel_system.rb'


class AgentNode

	include PackTypeDefine
	include ChannelSystem

	alias :_agent_node_initialize :initialize
	def initialize(*args)
		_agent_node_initialize(*args)
		self.init_channel
	end



	def send_channel(channel, &ret_proc)

		os = FSOutputStream.new
		os.write_channel(channel)
		pack = Pack.create(Pack.generate_serial, PACK_TYPE_CREATE_CHANNEL, os)
		self.send_pack(pack, nil, ret_proc)

	end

end