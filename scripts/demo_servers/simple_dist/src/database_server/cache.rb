
require 'redis'
require 'openssl'
require 'mysql2'

#==========================================================================================
# => redis集群管理器
#==========================================================================================
class CacheGroupManager

  attr_reader :redis_groups # redis 集群
  attr_reader :redis_group_keys # redis 集群hash组
  attr_reader :redis_real_group # 所有真實節點
  attr_reader :virtual_node_count

  def initialize(cache_addrs, virtual_node_count)

    @redis_groups = {}
    @redis_group_keys = []
    @redis_real_group = []
    @virtual_node_count = virtual_node_count;

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
      redis = Redis.new(:host => ip, :port => port);
      @redis_real_group << redis
    rescue => e
      redis = nil
    end

    # 创建虚拟节点
    for i in 0..@virtual_node_count
      hash = HashCode.hash(addr + "#{i}")
      @redis_groups[hash] = redis
    end

  end


  #==========================================================================================
  # => 持久化所有Cache
  #==========================================================================================
  def save()
    for redis in @redis_real_group
      redis.save()
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
      if (@redis_group_keys[i] >= hash)
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
    if (tail - start == 1)
      return start
    end
    if (tail - start == 0)
      return start
    end
    if (mid > v)
      find(keys, v, start, tail / 2)
    elsif mid < v
      find(keys, v, start + tail / 2, tail)
    else
      mid == v
      return start + tail / 2
    end
  end


  def sadd(set, args)
    cache = get_cache_from_key(set)

    # 无法获取到节点
    if (cache.nil?)
      raise("redis groups is empty")
      return nil;
    end
    begin
      m = cache.sadd(set, args);
    rescue Redis::CannotConnectError => err
      raise err
      return nil;
    end

  end

  # 這個函數較特殊!!
  def hmset(hash, key, *args)
    cache = get_cache_from_key(hash)

    # 无法获取到节点
    if (cache.nil?)
      raise("redis groups is empty")
      return nil;
    end
    begin
      m = cache.hmset(hash, key, *args);
      rescue Redis::CannotConnectError => err
      raise err
      return nil;
    end
    return m
  end


  #==========================================================================================
  # => 如果调用了自己不存在的方法,尝试从模板中调用
  #==========================================================================================
  def method_missing(method_name, *arg, &block)

    cache = get_cache_from_key(arg[0])

    # 无法获取到节点
    if (cache.nil?)
      raise("redis groups is empty")
      return nil;
    end
    begin
        cache.send(method_name, *arg)
    rescue Redis::CannotConnectError => err
      raise err
      return nil;
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


end



class HashCode
	class << self
		def hash(string)
			OpenSSL::HMAC.hexdigest("SHA1", "boomman", string).to_i(16) % 0xffffffff
		end
	end
end