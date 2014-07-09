
require 'ext/game_system.rb'
require 'ext/home/home_map.rb'
require 'ext/home/home_monster.rb'
require 'ext/home/home_player.rb'
require 'ext/home/home_cmd.rb'


class CMDChatMsg < Pack	
	class << self
		
		def chat_command_61home_enter(sender, args)
			
			$game_homes.handle_enter_home(sender, args[0].to_i, 0);
			
		end

		def chat_command_61home_gain_monster(sender, args)
			player = sender.player
			home = player.home
			if(args.size == 1)
				args << 1
			end
			for i in 0...args[1].to_i
				home.gain_monster(args[0].to_i)
			end
		end
		
		# usage /61home
		def chat_command_61home(sender, args)
			player = sender.player
			home = player.home
			$game_homes.handle_get_home(sender, 0)
		end
		
		# usage /61home_unlockmap id
		def chat_command_61home_unlockmap(sender, args)
			$game_homes.handle_unlock_map(sender, args[0].to_i, 0);
		end

		# usage /61home_upgrademap serial
		def chat_command_61home_upgrademap(sender, args)
			$game_homes.handle_upgrade(sender, args[0].to_i, 0);
		end

		# usage /61home_setmonster index serial
		def chat_command_61home_setmonster(sender, args)
			$game_homes.handle_set_monster(sender, args[0].to_i, args[1].to_i, 0);
		end
		
		# usage /61home_mixmonster monster_id
		def chat_command_61home_mixmonster(sender, args)
			$game_homes.handle_mix_monster(sender, args[0].to_i, 0);
		end
		
		# usage /61home_getxml map_id
		def chat_command_61home_getxml(sender, args)
			$game_homes.handle_download_xml(sender, args[0].to_i, 0);
		end
		
		# usage /61home_gaintech tech_id
		def chat_command_61home_gaintech(sender, args)
			$game_homes.handle_gain_tech(sender, args[0].to_i, 0);
		end
		
		# usage /61home_upgradetech tech_id
		def chat_command_61home_upgradetech(sender, args)
			$game_homes.handle_upgrade_tech(sender, args[0].to_i, 0);
		end
		
		# usage /61home_mixreq monster_id
		def chat_command_61home_mixreq(sender, args)
			$game_homes.handle_mix_monster_req(sender, args[0].to_i, 0);
		end
		
		# usage /61home_match
		def chat_command_61home_match(sender, args)
			$game_homes.handle_match(sender, 0);
		end
		
		# usage /61home_setmap mid
		def chat_command_61home_setmap(sender, args)
				
			rails("因为要设置守卫..这里暂时取消");
		
		end
		
		
		
		
		
	end
end


class GameManagerResoucre
	
	alias :old_reload_items :reload_items
	
	def reload_items
		result = old_reload_items();
		
		result << GameHomeSystem::HomeMonsterMix
		result << GameHomeSystem::HomeMonster
		result << GameHomeSystem::HomeMap
		result << GameHomeSystem::HomeTechnology
		
		
		return result
	end
	
end


class GameHomeSystem < GameSystemBase
	
	attr_reader :configure					# 家园全局配置
	attr_reader :global_homes				# 全局家园表		{	PID	=>	Home} 
	attr_reader :global_rank_list		# 全局排行列表	{ 分段 => [ PID, PID ]  }
	
	
	# 匹配一个可战斗对象
	# 返回一个PID
	def match(pid)
		
		# 得到home
		home = load_home(pid)
		# 得到分段
		section = get_rank_section(home.rank)
		# 得到分段组
		ret_pid = 0
		while (section >= 0) and (ret_pid == 0)
			
			@global_rank_list[section] ||= []
			section_group = @global_rank_list[section]
			
			# 算出差集
			sub_set = section_group - home.matched_pids
			# 减去自己
			sub_set.delete(pid)
			# 在唯一的可能性中找到随机一个
			if(sub_set.size > 0)
				ret_pid = sub_set[ rand(sub_set.size) ]
			else
				section -= 1;
			end
			
		end
		
		# 添加到已匹配历史
		home.add_match_history(ret_pid);
		
		return ret_pid
	end
	
	# 不匹配间隔
	def match_interval()
		return @configure["match_interval"]
	end
	
	# 获取积分段
	def get_rank_section(rank)
		return rank / @configure["rank_section"]
	end
	
	# 更新积分
	def update_rank(pid, old_rank, new_rank)
		
		return if(pid == 0)
	
		old_section = get_rank_section(old_rank)
		new_section = get_rank_section(new_rank)
		
		@global_rank_list[old_section] ||= []
		@global_rank_list[new_section] ||= []
		
		
		# 如果分段发生改变
		if(old_section != new_section)
			# 从旧的分段组删除
			@global_rank_list[old_section].delete(pid)
			# 添加到新的分段组
			@global_rank_list[new_section] << pid
			
			self.save_global_rank_list();
		else
		
			if(!@global_rank_list[old_section].include?(pid))
				@global_rank_list[old_section] << pid
				
				self.save_global_rank_list();
				
			end
			
		end
	end
	
	# 加载一个home
	def load_home(pid)
		
		home = @global_homes[pid]
		
		if(home != nil)
			return home;
		end
		
		home = Home.create_from_redis(pid)
		
		if(home.nil?)
			home = Home.create_from_database(pid)
		end
		
		if(home.nil?)
			home = Home.new(pid)
			home.set_map(home.gain_map(@configure["default_map_id"]).serial, nil)
			
			for i in 0...@configure["default_monster"].size
				monster = home.gain_monster(@configure["default_monster"][i])
				home.set_monster(monster.serial, i);
			end
			home.rank = 0
			
		end
		
		@global_homes[pid] = home
		
		return home
		
	end
	
	# 保存全局积分表
	def save_global_rank_list
		$game_database.set( "game_homes:global_rank_list", @global_rank_list.to_yaml )
	end
	
	# 加载全局积分表
	def load_global_rank_list
		yaml = $game_database.get("game_homes:global_rank_list")
		if(yaml.nil?)
			@global_rank_list = {}
		else
			@global_rank_list = YAML.load(yaml);
		end
	end
	
	# 加载家园列表
	def load_homes
		@global_homes = {}
	end
	
	def reload_res
		
		GameHomeSystem::HomeMonsterMix.reload_templete()
		GameHomeSystem::HomeMonster.reload_templete()
		GameHomeSystem::HomeMap.reload_templete()
		GameHomeSystem::HomeTechnology.reload_templete()
		
	end
	
	def start(server)
		super(server)
		
		
		configure_file = File.open(File.dirname(__FILE__) + "/home.yaml");
		@configure = YAML.load(configure_file)
		configure_file.close();

		
		# 从redis中读取ranklist
		self.load_global_rank_list()
		self.load_homes()
		
	end
	
	def stop
		super()
		
		
	end
	
	
end