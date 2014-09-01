
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
	
	
	def method_missing(method_name, *arg, &block)
		begin
			m = @node.method(method_name)
			return m.call(*arg);
		rescue => msg
			server.err(msg.message);
			return super
		end

	end

  def shutdown

    pack = Pack.create_disconnect( @agent_id )
    @node.send_pack(pack);


  end
	
	def send_pack(pack)
		
		# print "AgentNode#send_pack #{@agent_serial} 1\n"
		
		agent_pack = Pack.create_agent(@agent_id, pack, @agent_serial);
		
		# p @node.class
		@node.send_pack(agent_pack);
		
		
		# print "AgentNode#send_pack #{@agent_serial} 2\n"
		
	end


end