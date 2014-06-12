# encoding: ASCII-8BIT

require 'cmds/boomman_pack_type.rb'
class CMDChatMsg < Pack

	CHAT_MSG_TYPE_SYSTEM = -1
	CHAT_MSG_TYPE_WORLD = 0
	
	class << self
		
		def command_response(sender, msg)
			
			pack = CMDChatMsg.create(CHAT_MSG_TYPE_SYSTEM, 0, "system", msg)
			sender.send_pack(pack);
			
		end
		
		# => usage /61item item_id count
		def chat_command_61item(sender, args)
			
			player = sender.player
			
			count = args.size == 1 ? 1 : args[1].to_i
			if(count > 0)
				player.gain_item(args[0].to_i, count)
			elsif (count < 0)
				player.lose_item(args[0].to_i, count.abs)
			end	
			
			
		end
	
		# => usage /61reload
		def chat_command_61reload(sender, args)
		end
	
		def chat_command_61money1(sender, args)
			player = sender.player
			player.gain_gold(args[0].to_i)
		end
		def chat_command_61money2(sender, args)
			player = sender.player
			player.gain_diamonds(args[0].to_i)
			
		end
		def chat_command_61money2(sender, args)
			player = sender.player
			player.gain_prestige(args[0].to_i)
			
		end
	
		# => usage /61hero hero_id count
		def chat_command_61hero(sender, args)
			player = sender.player
			
			hero = player.gain_hero(args[0].to_i)
			if(hero != nil)
				command_response(sender, "获得1个#{hero.name}");
			else
				command_response(sender, "hero id 错误");	
			end
			
		end
	
	
		# => 协议号	:PACK_TYPE_CHAT_MSG
		# => 聊天消息
		def execute(sender, server, type, msg)
		
			player = sender.player
		
			if(msg["/61"] != nil)
				args = msg.split(" ")
				command = args[0][1, args[0].length]
			
				begin
					method = self.method("chat_command_#{command}")
					method.call(sender, args[1, args.size() - 1]);
				rescue => e
					server.err(e.message)
				end
			
				return 
			end
		
			# 世界聊天, 全服广播
			if(type == CHAT_MSG_TYPE_WORLD)
				cmd = CMDChatMsg.create(type, player.pid, player.name, msg)
				for key, value in server.agent_nodes
					value.send_pack(cmd)
				end
			end
		
		end
	
		
	
		def create(type, sender_pid, sender_name, msg)
			
			os = FSOutputStream.new();
			os.write_byte(type);
			os.write_uint32(sender_pid);
			os.write_string(sender_name);
			os.write_string(msg)
			return Pack.create( 0, PACK_TYPE_CHAT_MSG, os );
			
		end
	
	end
	
end