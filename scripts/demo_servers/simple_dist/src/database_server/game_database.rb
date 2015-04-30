require 'database_server/cache.rb'
class GameDatabase

  attr_reader :sql_connect
  attr_reader :cache
  attr_reader :configure

  def initialize(server, configure)

		@server = server
		@configure = configure

    init_caches
    init_db_connect

  end

  def init_caches()

		@cache = CacheGroupManager.new(@configure["redis_configure"], @configure["redis_virtual_count"])

  end

  def init_db_connect()

      @sql_connect = Mysql2::Client.new(
          :host => @configure["mysql_configure"]["host"],
          :username => @configure["mysql_configure"]["username"],
          :password => @configure["mysql_configure"]["password"],
          :database => @configure["mysql_configure"]["database"],
          :port => @configure["mysql_configure"]["port"])

  end

  def query(sql, connect=@sql_connect)
    result = nil
    begin
      result = connect.query(sql)
    rescue Mysql2::Error => err
      # 主键重复
      if err.error_number == 1062 || err.error_number == 1169
        return nil
      end
      # 语法错误
      if err.error_number == 1064
	      @server.err(" sql syntax error : #{sql} ")
        return nil
      end

      # 网络错误,重新连接
      if err.error_number == 1023 or
      				err.error_number == 1079 or
      				err.error_number == 1080 or
      				err.error_number == 1081 or
      				err.error_number == 1129 or
      				err.error_number == 1043 or  # 无效连接
      				err.error_number == 2003  # 无效连接

	    @server.err(err.message)

      init_db_connect
      query(sql, connect)
      end

    end
    result
  end


  # 持久化缓存
  def cache2storage(key, value)
    if value.class == String
      value = @sql_connect.escape(value)
    end # delayed
    sql = "replace delayed into tb_redis(r_key, r_value) values('#{key}', '#{value}')"
    query(sql)
  end

  # 從DB中讀取一條數據
  def load_data_from_db(key)
    sql = "select r_key,r_value from tb_redis where r_key='#{key}' limit 0,1"
    result = query(sql)
    value = nil
    if result and result.size > 0
      result.each do |row|
        value = row["r_value"]
      end
    end
    value
  end

  # 从DB读取cache
  def storage2cache(key)
    value = load_data_from_db(key)
    if value != nil
      set(key, value, false)
    end
    value
  end




  # 獲取HASH集合數據
  def hgetall(hash)
	  ret = @cache.hgetall(hash) # 先嘗試從CACHE中讀取
	  if ret.empty? # 如果是空的..測試多一次在db中讀取
		  set_data = load_data_from_db(hash)
		  # 緩存到cache
		  if set_data != nil
			  ret = eval(set_data)
			  args = []
			  for k, v in ret
				  args << k << v
			  end
			  if ret.size >= 2
				  @cache.hmset(hash, args)
			  end
		  end
	  end
		ret
  end

  # 設置HASH
  def hset(hash, key, value)
    key = key.to_s
    value = value.to_s
    @cache.hset(hash, key, value)
    cache2storage(hash, hgetall(hash).to_s)
  end

  def hget(hash, key)
	  @cache.hget(hash, key)
  end

  # 獲取集合數據.
  def smembers(set)
	  ret = @cache.smembers(set) # 先嘗試從CACHE中讀取
	  if ret.empty? # 如果是空的..測試多一次在db中讀取
		  set_data = load_data_from_db(set)
		  if set_data != nil
			  ret = eval(set_data)
			  for r in ret
				  @cache.sadd(set, r)
			  end
		  end
	  end
	  ret
  end

  # 往集合加入數據
  def sadd(set, v)
    v = v.to_s
    if (ret)
      # cache, DB 各一份
      @cache.sadd(set, v)
      cache2storage(set, smembers(set).to_s)
    end
    v
  end

  # 從集合中移除數據
  def srem(set, v)
    v = v.to_s
    ret = @cache.srem(set, v)
    if (ret)
      cache2storage(set, set_memery_cache(set).to_s)
    end
    ret
  end

  # 获取一个值
  def get(key)

    value = @cache.get(key)

    if value.nil?
      value = storage2cache(key)
    end

    value
  end

  # 设置一个值
  def set(key, value, storage=true)
    if storage
      cache2storage(key, value)
    end
    @cache.set(key, value)
  end

  # 获取自增值
  def incr(key)
		value = @cache.incr(key)
		set(key, value)
		value
  end


  def zadd(set, score, key)
    @cache.zadd(set, score, key)
  end

  def zrank(set, key)
    @cache.zrank(set, key)
  end

  def zrevrank(set, key)
    @cache.zrevrank(set, key)
  end

  def zrange(set, start, stop)
    @cache.zrange(set, start, stop)
  end

  def zrevrange(set, start, stop, options={})
    @cache.zrevrange(set, start, stop, options)
  end

end
