
require "net/https"  
require "uri"
require 'base64'


class Hash
	def +(target)
		for key , value in target
			self[key] = value	
		end
		return self
	end	
end


module QQ_API_PATH

	QQ_BASE_URI = "http://119.147.19.43"
	QQ_GROUP_GET_INFO = "/v3/user/get_info"
	QQ_GROUP_SEND_MSG = "/v3/qqgroup/send_msg"
	QQ_GROUP_GET_GROUP_INFO = "/v3/qqgroup/get_group_info"
	QQ_GROUP_GET_FRIENDS_INFO = "/v3/relation/get_app_friends"
	
	MPAY_BUY_GOODS_M = "/mpay/buy_goods_m"
	
end

#==========================================================================================
#   QQAPI封装
# 	By Frodo	2014-06-10
#==========================================================================================
class QQAPI
	
	include QQ_API_PATH
	
	# 炸弹APP
	APP_ID = "101096343"
	APP_KEY = "34c6c9b112be0b65fb4f4fae58003c8b"
	
	
	attr_reader :app_id
	attr_reader :app_key

	def encode(str)
		str = URI.encode_www_form_component(str)
		str = str.gsub("*") { |g| g = "%2A" }
		return str
	end
	

	#==========================================================================================
	# => 初始化
	#==========================================================================================
	def initialize(app_id=APP_ID, app_key=APP_KEY)
		@app_id = app_id
		@app_key = app_key
	end
	

	#==========================================================================================
	# => hamc + base64 处理
	#==========================================================================================
	def hamc_sha1(value, key)
		hmac = OpenSSL::HMAC.digest("SHA1", key, value) 
		signature = Base64.encode64 ( hmac )
		return signature.strip
	end	
	
	
	
	
	#==========================================================================================
	# => 获取个人信息
	# => gid 群openid
	#==========================================================================================
	def get_info(open_id, open_key)
		parm = {
		}
		
		return call_api(open_id, open_key, QQ_GROUP_GET_INFO, parm, "POST")
	end
	#==========================================================================================
	# => 获取好友
	# => gid 群openid
	#==========================================================================================
	def get_app_friends(open_id, open_key)
		parm = {
		}
		
		return call_api(open_id, open_key, QQ_GROUP_GET_FRIENDS_INFO, parm, "POST")
	end
	
	#==========================================================================================
	# => 获取群信息
	# => gid 群openid
	#==========================================================================================
	def get_group_info(open_id, open_key, gid)
		
		parm = {
			"group_openid" => gid, 
		}
		
		return call_api(open_id, open_key, QQ_GROUP_GET_GROUP_INFO, parm, "POST")
		
	end

	#==========================================================================================
	# => 同步qq群消息
	# => gid 群openid
	# => uid 工会ID
	# => msg 消息
	#==========================================================================================
	def group_send_msg(open_id, open_key, gid, uid, msg)
		
		parm = {
			"group_openid" => gid, 
			"msg" => msg, 
			"union_id" => uid
		}
		
		return call_api(open_id, open_key, QQ_GROUP_SEND_MSG, parm, "POST")
		
	end
	
	#==========================================================================================
	# => 根据参数, 路径, 请求方法生成sig
	#==========================================================================================
	def generate_sig(path, parm, method)
		parm_encode = ""
		for key in parm.keys.sort
			parm_encode = parm_encode + "#{key}=#{parm[key]}&"
		end
		parm_encode[parm_encode.length-1]=''
		requ_methods = method
		path_encode = encode(path)
		parm_encode = encode(parm_encode)
		sig_org = "#{requ_methods}&#{path_encode}&#{parm_encode}";
		sig_org = hamc_sha1(sig_org, APP_KEY + "&");
		return sig_org
	end
	
	
	#==========================================================================================
	# => 调用米大师api
	#==========================================================================================
	def call_mdas_api(open_id, open_key, pay_token, zoneid, pf, pfkey)
		
		parm = {
			"pf" => pf,
			"ts" => Time.now.to_i,
			"payitem" => "1*1*1000",
			"goodsmeta" => "name*desc",
			"goodsurl" => "http://imgcache.qq.com/qzone/space_item/pre/0/66768.gif",
			"zoneid" => zoneid, 
			"pfkey" => pfkey,
			"pay_token" => pay_token,
			"app_metadata" => "customkey"
		}
		
		cookie = {
			"session_id" => URI.encode_www_form_component("openid"),
			"session_type" => URI.encode_www_form_component("kp_actoken"),
			"org_loc" => URI.encode_www_form_component(MPAY_BUY_GOODS_M)
		}
		
		
		return call_api(open_id, open_key, MPAY_BUY_GOODS_M, parm, "GET", cookie)
	end
	
	#==========================================================================================
	# => 调用OpenQQAPI
	#==========================================================================================
	def call_api(open_id, open_key, path, api_parm, method, cookie={})
		
		uri = URI.parse(QQ_BASE_URI + path)
		
		base_parm = { 
			"appid" => APP_ID,
			"pf" => "qzone",
			"format" => "json",
			"openid" => open_id,
			"openkey" => open_key,
		}
		parm = base_parm + api_parm
		
		
		parm["sig"] = generate_sig(path, parm, method);

		for k, v in parm
			parm[k] = encode(v)
		end

		http = Net::HTTP.new(uri.host, uri.port)
		
		if(method == "POST")
			request = Net::HTTP::Post.new(uri.request_uri)
			request.set_form_data(parm)
		elsif (method == "GET")
			
			params = uri.request_uri + "?"
			for k, v in parm
				params += "#{k}=#{v}&"
			end
			params[params.length - 1] = ""
			request = Net::HTTP::Get.new(params)
		end
		
		coockie = String.new
		for key, value in cookie
			coockie = coockie + "#{key}=#{value};"
		end
		request["Cookie"] = coockie
		response = http.request(request)
		
		return response.body
		
	end
	
end



qqapi = QQAPI.new("101096343", "34c6c9b112be0b65fb4f4fae58003c8b")
open_id = "A1070233F2755B5E5D167E943513153D"
open_key = "4E7ED0450D0BB2D0FF3779AF973E8C1E"

openid = "A1070233F2755B5E5D167E943513153D"
access_token = "4E7ED0450D0BB2D0FF3779AF973E8C1E"
pay_token = "794B3D8494951F6F1E11E03197675FBC"
pf = "desktop_m_qq-10000144-android-2002-"
pfkey = "9fb298c914c04f6aae9770140488ab26"



# gid = "DCEBF31A55AB9E08B2149D4319E6DFFA"
# ret = qqapi.call_mdas_api(open_id, open_key, pay_token, 1, pf, pfkey)

# print ret


# ret = qqapi.get_info(open_id, open_key)
# ret = qqapi.group_send_msg(open_id, open_key, gid, 1, "hi")
# ret = qqapi.get_group_info(open_id, open_key, gid)
# ret = qqapi.get_app_friends(open_id, open_key)
# p ret