

require 'pack_type.rb'
require 'util/uuid.rb'
require 'util/hash.rb'
require 'channel/channel_system.rb'


class AgentNode

	include PackTypeDefine
	include ChannelSystem

	alias :_agent_node_initialize :initialize
	def initialize(*args)
		_agent_node_initialize(*args)
		self.init_entities
	end

end