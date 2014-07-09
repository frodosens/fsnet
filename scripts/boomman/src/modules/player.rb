# encoding: UTF-8
require 'modules/basemode.rb'
#==========================================================================================
#   玩家实例
# 	By Frodo	2014-06-10
#==========================================================================================
class Player < BaseModule
	
	TEMP_DATA_KEY = :TEMP_DATA
	
	class << self
	
		def attr_user(name)
			define_method("#{name}=") do |args|
				return set_data(name, args)
			end
			define_method("#{name}") do
				return get_data(name)
			end
		end
	
		def attr_user_temp(name)
			define_method("#{name}=") do |args|
				set_tmp_data(name, args)
			end
			define_method("#{name}") do
				tmp_data(name)
			end
		end
	
	end
	
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
	attr_reader :face			# 头像

	attr_user :last_logout_time	# 上次登出时间
	attr_user_temp :pve_state

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
		@client   = nil
		@face     = ""
	end
	
		
	
	#==========================================================================================
	# => 头像
	#==========================================================================================
	def face
		return "local://Role.png"
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
		@level = 1
		gain_hero(1, false);
	end
	
	
	
	#==========================================================================================
	# => 设置数据(持久保存)
	#==========================================================================================
	def set_data(key, value)
		if(key == TEMP_DATA_KEY)
			raise("the key is already used")
		end
		return (@user_data[key] = value)
	end
	
	#==========================================================================================
	# => 获取数据(持久保存)
	#==========================================================================================
	def get_data(key)
		return @user_data[key]
	end
	
	#==========================================================================================
	# => 设置临时数据(每日清理)
	#==========================================================================================
	def set_tmp_data(key, value)
		@user_data[TEMP_DATA_KEY][key] = value
	end
	#==========================================================================================
	# => 设置临时数据(每日清理)
	#==========================================================================================
	def tmp_data(key)
		return @user_data[TEMP_DATA_KEY][key]
	end
	
	
	#==========================================================================================
	# => 清除每日数据
	#==========================================================================================
	def clear_daily_data()
	
	end
	
	#==========================================================================================
	# => 当登入时的回调
	#==========================================================================================
	def on_login()
		
		# 清除每日缓存数据
		@user_data[TEMP_DATA_KEY] ||= {}
		tmp_data_day = @user_data[TEMP_DATA_KEY]["yday"].to_i
		today = Time.now.yday
		if(tmp_data_day != today)
			@user_data[TEMP_DATA_KEY] = {}
			@user_data[TEMP_DATA_KEY]["yday"] = today
			# 清楚每日数据
			clear_daily_data()
		end
		
		
	end
	
	#==========================================================================================
	# => 当登出时的回调
	#==========================================================================================
	def on_logout()
	
		# 记录登出时间
		self.last_logout_time = Time.now
		
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
	# => 接收邮件
	#==========================================================================================
	def recv_mail(mail)
		mail.recv_pid = self.pid
		@mails << mail
		
		send_pack(CMDMail.create_recv_main(mail));
		
	end
	
	#==========================================================================================
	# => 找到英雄
	#==========================================================================================
	def find_hero_by_serial(serial)
		hero = @heros[serial];
		if(hero != nil and hero.deleted?)
			hero = nil;
		end
		return hero;
	end
	
	#==========================================================================================
	# => 找到邮件
	#==========================================================================================
	def find_mail_by_serial(serial)
		for mail in @mails
			if(mail.serial == serial and !mail.deleted?)
				return mail
			end
		end
		return nil
	end
	
	#==========================================================================================
	# => 根据序列号找到道具
	#==========================================================================================
	def find_item_by_serial(serial)
		return find_item(serial, :by_serial => true);
	end
	
	#==========================================================================================
	# => 获取道具数量
	#==========================================================================================
	def item_count(item_id)

		count = 0
		for it in items
				
			if(it.id == item_id && !it.deleted?)
				count += it.stack;
			end
				
		end
		
		return count
	end
	
	#==========================================================================================
	# => 得到道具
	#==========================================================================================
	def gain_item(item_id, item_count)
		
		return if(item_id == 0)
		
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
			if(new_item != nil)
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
			else
				break;
			end
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
				if(item.deleted?)
					deleted_items << item.serial;
				else
					changed_items << item;	
				end
			else
				item.stack -= can_be_lose_count
				count -= can_be_lose_count;
				if(item.deleted?)
					deleted_items << item.serial;
				else
					changed_items << item;	
				end
			end
		end
		
		
		
		cmd = CMDItemUpdate.create_item_update([], changed_items, deleted_items)
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
		send_pack( CMDPropertyUpdate.create_diamonds_udpate(@diamonds)  );
		return @diamonds
	end
	#==========================================================================================
	# => 得到声望
	#==========================================================================================
	def gain_prestige(val)
		@prestige += val
		@prestige = ( @prestige < 0 ? 0 : @prestige);
		@prestige = ( @prestige > $game_configure["boomman_configure"]["max_prestige"] ? $game_configure["boomman_configure"]["max_prestige"] : @prestige);
		send_pack( CMDPropertyUpdate.create_prestige_update(@prestige)  );
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
	def lose_diamonds(val)
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
		return @@exp_tables[@level];
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
	# => 通知客戶端更新一个英雄卡片
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
		os.write_string(self.face);
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
		super()
		cache_simple()
	end

	#==========================================================================================
	# => 缓存这个player的simple数据
	#==========================================================================================
	def cache_simple
		
		simple_data = {
			:pid => @pid,
			:name => @name,
			:level => @level,
			:sex => @sex,
			:face => self.face
		}
		
		simple_key = Player.generate_simple_cache_key(@pid)
		$game_database.set( simple_key, simple_data.to_yaml() )
		
	end
	
	#==========================================================================================
	# => 生成update的sql
	#==========================================================================================
	def generate_update_sql()
		
		data = user_data.to_yaml()
		data = SafeSql.conver(data)
		sql = "update tb_player set level=#{@level}, sex=#{@sex}, name='#{@name.force_encoding("UTF-8")}', morale=#{@morale}, ap=#{@ap}, exp=#{@exp}, gold=#{@gold}, diamonds=#{@diamonds}, prestige=#{@prestige}, guild_id=#{@guild_id}, user_data='#{data}' where pid=#{@pid}  "
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
	# => 检查应该删除的items
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
		for item in will_delete_items
			@items.delete(item);
		end
	end
	
	#==========================================================================================
	# => 检查应该删除的mail
	#==========================================================================================
	def check_delete_mail()
		
		will_delete_mails = []
		for mail in @mails
			# 如果已经被删
			if(mail.deleted?)
				will_delete_mails << mail
			end
		end
		# 删除应该被删除的mail
		for mail in will_delete_mails
			@mails.delete(mail);
		end
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
		end
		for item in @items
			sqls << item.generate_save_sql();
		end
		for mail in @mails
			sqls << mail.generate_save_sql();
		end
		
		# 不是任何连接客户端.
		
		$game_database.try_remoot_execute(sqls);
		
		check_delete_hero();
		check_delete_item();
		check_delete_mail();
		
	end
	#==========================================================================================
	#    类方法 
	# 
	#==========================================================================================
	class << self
		CACHE_KEY_VERSION = 1
		
		@@exp_tables = []
		def reload_templete
			@@exp_tables = [ 0 ]
			
			CSV.load_csv("res/tmp_role_level.csv") do |hash|
				@@exp_tables[hash["level"].to_i] = hash["exp"].to_i
			end
			
		end
		
		
		#==========================================================================================
		# => 从cache生成简要数据
		#==========================================================================================
		def create_simple_from_redis(pid)
			
			# 如果在线,直接返回
			if($game_players.is_online(pid))
				return $game_players.find_player_by_pid(pid)
			end
			
			key = Player.generate_simple_cache_key(pid)
			yaml = $game_database.get(key);
			if(yaml != nil)
				hash = YAML.load(yaml)
				player = new()
				player.init_from_hash(hash)
				return player;
			end
			
			# 如果没有...只能去拿全数据了
			player = create_from_redis(pid)
		
			if(player != nil)
				player.cache_simple();
			end
			
			return player;
		end
		
		#==========================================================================================
		# => 根据PID生成缓存KEY
		#==========================================================================================
		def generate_cache_key(pid)
			return "players:#{CACHE_KEY_VERSION}:#{pid}";		
		end
		#==========================================================================================
		# => 根据PID生成Simple缓存KEY
		#==========================================================================================
		def generate_simple_cache_key(pid)
			return "players:simple:#{CACHE_KEY_VERSION}:#{pid}";		
		end
		#==========================================================================================
		# => 通过redis创建一个player
		#==========================================================================================
		def create_from_redis(pid)
			key = generate_cache_key(pid);
			yaml = $game_database.get(key);
			if(yaml != nil)
				return YAML.load(yaml);
			end
		end
		
		
		#==========================================================================================
		# => 通过db创建一个player
		#==========================================================================================
		def create_from_database(pid)
			
			player = nil;
			
			sql = "select * from tb_player where pid=#{pid} limit 0,1"
			result = $game_database.query(sql);
			if(result and result.size > 0)
				player = new();
				result.each do |row|
					player.init_from_hash(row)
				end
			end
			
			if(player)
				
				#==============================================================================
				# 加载英雄数据
				#==============================================================================
				sql = "select * from tb_heros where owner_pid=#{pid} and deleted=0"
				result = $game_database.query(sql);
				if(result and result.size > 0)
					result.each do |row|
						hero = Hero.new()
						hero.init_from_hash(row)
						player.heros[hero.serial] = hero
					end
				end
				#==============================================================================
				
				
				#==============================================================================
				# 加载道具数据
				#==============================================================================
				sql = "select * from tb_items where owner_pid=#{pid} and deleted=0"
				result = $game_database.query(sql);
				if(result and result.size > 0)
					result.each do |row|
						item = Item.new()
						item.init_from_hash(row)
						player.items << item
					end
				end
				#==============================================================================
				
				
				#==============================================================================
				# 加载邮件
				#==============================================================================
				sql = "select * from tb_mail where recv_pid=#{pid} and deleted=0"
				result = $game_database.query(sql);
				if(result and result.size > 0)
					result.each do |row|
						mail = Mail.new()
						mail.init_from_hash(row)
						player.mails << mail
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

