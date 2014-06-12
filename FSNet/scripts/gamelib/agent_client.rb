
class AgentNode
	
	attr_reader :server
	attr_reader :node
	attr_reader :agent_id
	def initialize(server, node, agent_id)
		@server = server;
		@node = node;
		@agent_id   = agent_id;
	end
	
	
	def method_missing(method_name, *arg, &block)
		begin
			m = @node.method(method_name)
			m.call(*arg);
		rescue => msg
			server.err(msg.message);
		end
	end
	
	
	def send_pack(pack)
		
		agent_pack = Pack.create_agent(@agent_id, pack);
		
		@node.send_pack(agent_pack);
		
	end


end