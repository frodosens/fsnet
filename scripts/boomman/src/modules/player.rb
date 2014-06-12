#==========================================================================================
#   玩家实例
# 	By Frodo	2014-06-10
#==========================================================================================
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
	attr_accessor :client # 绑定客户端
	

	#==========================================================================================
	# => 初始化
	#==========================================================================================
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
		@heros    = {}
		@items    = []
		@friends  = []
		@mails    = []
		@guild_id = 0
		@client  = nil
	end
	
	#==========================================================================================
	# => 排除一些不需要参与序列化的字段
	#==========================================================================================
  def to_yaml_properties
		
		propertirs = super;
		propertirs.delete( :@client );
		
		return propertirs
	end
	
	
	#==========================================================================================
	# => 初始化任务
	# => sex : 0 为男性  1 为女性
	#==========================================================================================
	def init(sex, name)
		@sex = sex
		@name = name;
		gain_hero(1, false);
	end
	
	
	#==========================================================================================
	# => 根据道具ID找到物品堆
	# => opt{
	# =>   :can_stack => true/false 
	# =>   :by_serial	=> true/false	
	# => }
	#==========================================================================================
	def find_item(key, option={})
		for it in items
			
			cond = (!it.deleted?)
			
			if(option[:by_serial])
				cond &= (it.serial == key)
			else
				cond &= (it.id == key)
			end
			
			if(option[:can_stack])
				cond &= it.stack < it.max_stack
			end
			
			if(cond)
				return it;
			end
		end
		return nil;
	end
	
	#==========================================================================================
	# => 根据序列号找到道具
	#==========================================================================================
	def find_item_by_serial(serial)
		return find_item(serial, :by_serial => true);
	end
	
	#==========================================================================================
	# => 得到道具
	#==========================================================================================
	def gain_item(item_id, item_count)
		
		change_items = []
		new_items = []
		
		count = item_count
		# 不断找可以堆叠的道具堆
		while( (item = find_item(item_id, :can_stack=>true)) != nil and count > 0 )
			
			can_be_add_stack = item.max_stack - item.stack
			# 如果有现有的道具可叠加
			if(can_be_add_stack > count)
				item.stack += count;
				count = 0
			else
				item.stack += can_be_add_stack;
				count -= can_be_add_stack;
			end
			change_items << item
			
		end
		# 如果无法堆到现有的道具上
		# 因为有可能数量很大,但是组大堆叠是1, 所以要while
		while(count > 0)
			new_item = Item.create_from_id(item_id)
			if(count > new_item.max_stack)
				new_item.stack = new_item.max_stack
				count -= new_item.max_stack;
			else
				new_item.stack = count
				count = 0;
			end
			new_item.owner_pid = self.pid
			@items << new_item;
			new_items << new_item
		end
		
		cmd = CMDItemUpdate.create_item_update(new_items, change_items, [])
		send_pack(cmd);
	end
	
	
	#==========================================================================================
	# => 失去道具
	#==========================================================================================
	def lose_item_by_serial(item_serial, item_count)
		lose_item(item_serial, item_count, :by_serial=>true)
	end
	
	
	#==========================================================================================
	# => 失去道具
	#==========================================================================================
	def lose_item(item_id, item_count, opt={})
		changed_items = []
		deleted_items = []
		count = item_count
		while( (item = find_item(item_id, opt)) != nil and count > 0 )
			can_be_lose_count = item.stack
			if(can_be_lose_count >= count)
				item.stack -= count;
				count = 0
				if(items.deleted?)
					deleted_items << item;
				else
					changed_items << item;	
				end
			else
				item.stack -= can_be_lose_count
				count -= can_be_lose_count;
				if(items.deleted?)
					deleted_items << item;
				else
					changed_items << item;	
				end
			end
		end
		
		
		
		cmd = CMDItemUpdate.create_item_update([], change_items, deleted_items)
		send_pack(cmd);
		
	end
	
	#==========================================================================================
	# => 得到经验值
	#==========================================================================================
	def gain_exp(exp)
		@exp += exp;
	end
	
	#==========================================================================================
	# => 发送一个包给这个玩家的客户端
	#==========================================================================================
	def send_pack(pack)
		if(@client != nil)
			
			@client.send_pack(pack);			
			
		end
	end
	#==========================================================================================
	# => 得到金币
	#==========================================================================================
	def gain_gold(val)
		@gold += val
		@gold = ( @gold < 0 ? 0 : @gold);
		@gold = ( @gold > $game_configure["boomman_configure"]["max_gold"] ? $game_configure["boomman_configure"]["max_gold"] : @gold);
		send_pack( CMDPropertyUpdate.create_gold_update(@gold)  );  
		return @gold
	end
	#==========================================================================================
	# => 得到钻石
	#==========================================================================================
	def gain_diamonds(val)
		@diamonds += val
		@diamonds = ( @diamonds < 0 ? 0 : @diamonds);
		@diamonds = ( @diamonds > $game_configure["boomman_configure"]["max_diamonds"] ? $game_configure["boomman_configure"]["max_diamonds"] : @diamonds);
		send_pack( CMDPropertyUpdate.create_diamonds_udpate(@gold)  );
		return @diamonds
	end
	#==========================================================================================
	# => 得到声望
	#==========================================================================================
	def gain_prestige(val)
		@prestige += val
		@prestige = ( @prestige < 0 ? 0 : @prestige);
		@prestige = ( @prestige > $game_configure["boomman_configure"]["max_prestige"] ? $game_configure["boomman_configure"]["max_prestige"] : @prestige);
		send_pack( CMDPropertyUpdate.create_prestige_update(@gold)  );
		return @prestige
	end
	
	#==========================================================================================
	# => 失去金币
	#==========================================================================================
	def lose_gold(val)
		return gain_gold(-val);
	end
	#==========================================================================================
	# => 失去钻石
	#==========================================================================================
	def lsoe_diamonds(val)
		return gain_diamonds(-val);
	end
	#==========================================================================================
	# => 失去声望
	#==========================================================================================
	def lose_prestige(val)
		return gain_prestige(-val);
	end
	#==========================================================================================
	# => 当前升级所需的经验
	#==========================================================================================
	def level_exp()
		return 100;
	end
	
	#==========================================================================================
	# => 通过序列号找到一个hero
	#==========================================================================================
	def find_hero_by_serial(serial)
		hero = @heros[serial];
		if(hero != nil and !hero.deleted?)
			return hero
		end
		return nil
	end
	
	#==========================================================================================
	# => 得到1个英雄卡片
	#==========================================================================================
	def gain_hero(hero_id, nofit=true)
		
		hero = Hero.create_from_id(hero_id, self.pid);
		
		if(hero != nil)
			@heros[hero.serial] = hero;
			
			if(nofit)
				send_pack( CMDHeroUpdate.create_gain_hero(hero)  );
			end
			
			return hero
		end
		
		return nil
	end
	
	#==========================================================================================
	# => 失去一个英雄卡片
	#==========================================================================================
	def lose_hero(hero_serial, nofit=true)
		hero = find_hero_by_serial(hero_serial);
		if(hero)
			# 设置已经删除
			hero.deleted = true
			if(nofit)
				send_pack( CMDHeroUpdate.create_lose_hero(hero_serial)  );
			end
		end
	end
	#==========================================================================================
	# => 更新一个英雄卡片
	#==========================================================================================
	def update_hero(hero, nofit=true)
		if(nofit)
			send_pack( CMDHeroUpdate.create_update_hero(hero)  );
		end
	end
	
	
	
	#==========================================================================================
	# => 将player序列化到输出流
	#==========================================================================================
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
		
		os.write_uint16(@heros.size)
		for hero_serial in @heros.keys.sort
			@heros[hero_serial].write_to_stream(os);
		end
		
		os.write_uint16(@items.size)
		for item in @items
			item.write_to_stream(os);
		end
		
	end
	#==========================================================================================
	# => 获取这个player的缓存cache
	#==========================================================================================
	def cache_key
		return Player.generate_cache_key( @pid );
	end
	#==========================================================================================
	# => 缓存这个player
	#==========================================================================================
	def cache
		key = cache_key();
		$game_database.redis.set( key, self.to_yaml() )
	end
	
	#==========================================================================================
	# => 生成update的sql
	#==========================================================================================
	def generate_update_sql()
		sql = "update tb_player set level=#{@level}, sex=#{@sex}, name='#{@name}', morale=#{@morale}, ap=#{@ap}, exp=#{@exp}, gold=#{@gold}, diamonds=#{@diamonds}, prestige=#{@prestige}, guild_id=#{@guild_id} where pid=#{@pid}  "
		return sql
	end
	
	#==========================================================================================
	# => 检查应该删除的heros
	#==========================================================================================
	def check_delete_hero()
		
		will_delete_heros_serials = []
		for key, value in @heros
			# 如果已经被删除
			if(value.deleted?)
				will_delete_heros_serials << key
			end
		end
		# 删除应该被删除的hero
		for hero_serial in will_delete_heros_serials
			@heros.delete(hero_serial);
		end
		
	end
	
	#==========================================================================================
	# => 检查应该删除的heros
	#==========================================================================================
	def check_delete_item()
		
		will_delete_items = []
		for item in @items
			# 如果已经被删除
			if(item.deleted?)
				will_delete_items << item
			end
		end
		# 删除应该被删除的hero
		@items.delete(will_delete_items);
		
	end
	
	
	#==========================================================================================
	# => 持久化这个player
	#==========================================================================================
	def save
		
		will_delete_heros_serials = []
		
		sqls = []
		# 人物的更新
		sqls <<  generate_update_sql();
		# 生成所有hero应该执行的sql
		for key, value in @heros
			sqls << value.generate_save_sql();
			# 如果标记还是未入库, 修改标记
			if(!value.inserted)
				value.inserted = true
			end
		end
		for item in @items
			sqls << item.generate_save_sql();
			# 如果标记还是未入库, 修改标记
			if(!item.inserted)
				item.inserted = true
			end
		end
		
		
		# 不是任何连接客户端.
		if(self.client.nil?)
			$game_database.try_remoot_execute(nil, sqls);
		else
			$game_database.try_remoot_execute(self.client.server, sqls);
		end
		
		check_delete_hero();
		check_delete_item();
		
	end
	#==========================================================================================
	#    类方法 
	# 
	#==========================================================================================
	class << self
		CACHE_KEY_VERSION = 1
		#==========================================================================================
		# => 根据PID生成缓存KEY
		#==========================================================================================
		def generate_cache_key(pid)
			return "players:#{CACHE_KEY_VERSION}:#{pid}";		
		end
		#==========================================================================================
		# => 通过redis创建一个player
		#==========================================================================================
		def create_from_redis(pid)
			key = generate_cache_key(pid);
			yaml = $game_database.redis.get(key);
			if(yaml != nil)
				return YAML.load(yaml);
			end
		end
		#==========================================================================================
		# => 通过db创建一个player
		#==========================================================================================
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
			
			if(player)
				
				#==============================================================================
				# 加载英雄数据
				#==============================================================================
				sql = "select * from tb_heros where owner_pid=#{pid}"
				result = $game_database.query(sql);
				if(result and result.size > 0)
					result.each do |row|
						hero = Hero.new()
						for key, value in row
							if(value.class == String)
								hero.instance_eval( "@#{key}='#{value}'" );
							else
								hero.instance_eval( "@#{key}=#{value}" );
							end
						end
						# 从DB中拿出来的.
						hero.inserted = true
						player.heros[hero.serial] = hero
					end
				end
				#==============================================================================
				
				
				#==============================================================================
				# 加载道具数据
				#==============================================================================
				sql = "select * from tb_items where owner_pid=#{pid}"
				result = $game_database.query(sql);
				if(result and result.size > 0)
					result.each do |row|
						item = Item.new()
						for key, value in row
							if(value.class == String)
								item.instance_eval( "@#{key}='#{value}'" );
							else
								item.instance_eval( "@#{key}=#{value}" );
							end
						end
						# 从DB中拿出来的.
						item.inserted = true
						player.items << item
					end
				end
				#==============================================================================
				
				
			end
			
			if(player != nil)
				player.cache();
			end
			
			
			return player
			
			
		end
		
	end
	
end



