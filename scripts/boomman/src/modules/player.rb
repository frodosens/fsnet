
class Player
	
	attr_reader :pid			# PID
	attr_reader :level		# 等级
	attr_reader :sex			# 性别
	attr_reader :name			# 名字
	attr_reader :morale		# 士气
	attr_reader :ap				# 行动力
	attr_reader :exp			# 经验值
	attr_reader :gold			# 金币
	attr_reader :diamonds	# 钻石
	attr_reader :prestige	# 声望
	attr_reader :heros		# 英雄列表
	attr_reader :items		# 道具列表
	attr_reader :friends	# 好友列表
	attr_reader :mails		# 邮件列表
	attr_reader :guild_id	# 工会ID

	
	def initialize()
		@pid      = 0
		@level    = 0
		@sex      = -1
		@name     = ""
		@morale   = 0
		@ap				= 0
		@exp      = 0
		@gold     = 0
		@diamonds = 0
		@prestige = 0
		@heros    = []
		@items    = []
		@friends  = []
		@mails    = []
		@guild_id = 0
	end
	
	def write_to_stream(stream)
		
	end
	
	
	def init(sex, name)
		@sex = sex
		@name = name;
	end
	
	def find_item_by_id(item_id)
		for it in items
			if(it.id == item_id)
				return it;
			end
			
		end
		return nil;
	end
	
	def gain_item(item_id, item_count)
		item = find_item_by_id(item_id);
		# 如果有现有的道具可叠加
		if(item != NULL and item.stack < item.max_stack)
			item.stack += 1;
		end
	end
	
	def lose_item(item_id, item_count)
		
	end
	
	def gain_exp(exp)
		@exp += exp;
	end
	
	def gain_gold(val)
		@gold += val
		@gold = ( @gold < 0 ? 0 : @gold);
		@gold = ( @gold > $game_configure["boomman_configure"]["max_gold"] ? $game_configure["boomman_configure"]["max_gold"] : @gold);
		return @gold
	end
	
	def gain_diamonds(val)
		@diamonds += val
		@diamonds = ( @diamonds < 0 ? 0 : @diamonds);
		@diamonds = ( @diamonds > $game_configure["boomman_configure"]["max_diamonds"] ? $game_configure["boomman_configure"]["max_diamonds"] : @diamonds);
		return @diamonds
	end
	
	def gain_prestige(val)
		@prestige += val
		@prestige = ( @prestige < 0 ? 0 : @prestige);
		@prestige = ( @prestige > $game_configure["boomman_configure"]["max_prestige"] ? $game_configure["boomman_configure"]["max_prestige"] : @prestige);
		return @prestige
	end
	
	
	def lose_gold(val)
		return gain_gold(-val);
	end
	def lsoe_diamonds(val)
		return gain_diamonds(-val);
	end
	def lose_prestige(val)
		return gain_prestige(-val);
	end
	
	def sex=(value)
		@sex=value
	end
	def name=(value)
		@name = value
	end
	
	def level_exp()
		return 100;
	end
	
	def write_to_stream(os)
		
		os.write_uint32(@pid);
		os.write_uint16(@level);
		os.write_byte(@sex);
		os.write_string(@name);
		os.write_uint16(@morale);
		os.write_uint16(@ap);
		os.write_uint32(@exp);
		os.write_uint32(self.level_exp);
		os.write_uint32(@gold);
		os.write_uint32(@diamonds);
		os.write_uint32(@prestige);
		os.write_uint32(@guild_id);
		
	end
	
	def cache_key
		return Player.generate_cache_key( @pid );
	end
	
	def cache
		key = cache_key();
		$game_database.redis.set( key, self.to_yaml() )
	end
	
	def save
		
		sql = "update tb_player set level=#{@level}, sex=#{@sex}, name='#{@name}', morale=#{@morale}, ap=#{@ap}, exp=#{@exp}, gold=#{@gold}, diamonds=#{@diamonds}, prestige=#{@prestige}, guild_id=#{@guild_id} where pid=#{@pid}  "
		$game_database.query(sql);
		
	end
	
	class << self
		
		def generate_cache_key(pid)
			return "players:#{pid}";		
		end
		
	  
		
		def create_from_redis(pid)
			key = generate_cache_key(pid);
			yaml = $game_database.redis.get(key);
			if(yaml != nil)
				return YAML.load(yaml);
			end
		end
		
		def create_from_database(pid)
			
			player = nil;
			
			sql = "select * from tb_player where pid=#{pid}"
			result = $game_database.query(sql);
			if(result and result.size > 0)
				player = new();
				result.each do |row|
					for key, value in row
						if(value.class == String)
							player.instance_eval( "@#{key}='#{value}'" );
						else
							player.instance_eval( "@#{key}=#{value}" );
						end
					end
				end
			end
			
			if(player != nil)
				player.cache();
			end
			
			return player
			
		end
		
	end
	
end



