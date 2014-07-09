

#==========================================================================================
# => redis集群管理器
#==========================================================================================
class CacheGroupManager
	
	attr_reader :redis_groups			# redis 集群
	attr_reader :redis_group_keys	# redis 集群hash组
	
	def initialize(cache_addrs)
		
		@redis_groups = {}
		@redis_group_keys = []

		for addr in cache_addrs
			create_cache_node(addr)
		end
		
		@redis_group_keys = @redis_groups.keys.sort
		
	end
	
	
	#==========================================================================================
	# => 创建节点
	#==========================================================================================
	def create_cache_node(addr)
		ip = addr.split(":")[0]
		port = addr.split(":")[1].to_i
		
		redis = nil
		begin 
			redis = Redis.new( :host=> ip, :port => port);
		rescue => e
			redis = nil
		end

		# 创建虚拟节点
		for i in 0..2
			hash = HashCode.hash(addr + "#{i}")
			@redis_groups[hash] = redis
		end
			
	end
	
	#==========================================================================================
	# => 移除一个节点
	#==========================================================================================
	def remove_cache_node(addr)

		for i in 0..2
			hash = HashCode.hash(addr + "#{i}")
			@redis_groups.delete(hash)
		end
		
	end
	
	#==========================================================================================
	# => 找到最近的cache点
	#==========================================================================================
	def find_near_cache(hash)
		start = find(@redis_group_keys, hash, 0, @redis_group_keys.size - 1)
		for i in start...@redis_group_keys.size
			if(@redis_group_keys[i] >= hash)
				return @redis_groups[@redis_group_keys[i]]
			end
		end
		# 如果找了一轮..都找不到..
		return @redis_groups[@redis_group_keys.first]
	end
	
	
	#==========================================================================================
	# => 折中找到开始搜寻点
	#==========================================================================================
	def find(keys, v, start, tail)
		mid = keys[start + tail / 2]
		if(tail - start == 1)
			return start
		end
		if(tail - start == 0)
			return start
		end
		if(mid > v)
			find(keys, v, start, tail / 2)
		elsif mid < v
			find(keys, v, start + tail / 2, tail)
		else mid == v
			return start + tail / 2
		end
	end
	
	
	#==========================================================================================
	# => 通过key找到cache点
	#==========================================================================================
	def get_cache_from_key(key)
		
		hash = HashCode.hash(key)
		
		cache = find_near_cache(hash)
		
		return cache;
	end
	
	
	#==========================================================================================
	# => 自增一个key
	#==========================================================================================
	def incr(key)
		cache = get_cache_from_key(key)
		# 无法获取到节点
		if(cache.nil?)
			raise("redis groups is empty")
			return nil;
		end
		return cache.incr(key)
	end
	
	#==========================================================================================
	# => 设置一个value
	#==========================================================================================
	def set(key, value)
		cache = get_cache_from_key(key)
		# 无法获取到节点
		if(cache.nil?)
			raise("redis groups is empty")
			return nil;
		end
		return cache.set(key, value)
	end
	
	#==========================================================================================
	# => 获取一个vaue
	#==========================================================================================
	def get(key)
		cache = get_cache_from_key(key)
		# 无法获取到节点
		if(cache.nil?)
			raise("redis groups is empty")
			return nil;
		end
		begin
			value = cache.get(key)
		rescue Redis::CannotConnectError => err 
			$game.err(err.message)
			# 该节点报错, 直接移除,并重来
			remove_cache_node("#{cache.client.host}:#{cache.client.port}")
			return get(key);
		end
		
		return value
		
	end
	
	
	
	
end