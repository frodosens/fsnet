
class AgentNode
	
	attr_reader :server
	attr_reader :node
	attr_reader :agent_id
	attr_accessor :agent_serial
	def initialize(server, node, agent_id)
		@server = server;
		@node = node;
		@agent_id   = agent_id;
	end

	def id
		return @agent_id
	end

	def method_missing(method_name, *arg, &block)


		if @nodes.methods.include?(method_name)
			self.instance_eval(
					"def #{method_name}(*arg, &block)
						return @node.send(#{method_name.to_sym}, *arg)
				   end"
			)
			self.send(method_name, arg, &block)
		else
			super
		end

	end

  def shutdown

    pack = Pack.create_disconnect( @agent_id )
    @node.send_pack(pack);


  end
	
	def send_pack(pack, *args)

		agent_pack = Pack.create_agent(@agent_id, pack, @agent_serial);
		@node.send_pack(agent_pack, *args)

	end


end