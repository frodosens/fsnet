
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
	QQ_GROUP_SEND_MSG = "/v3/qqgroup/send_msg"
	QQ_GROUP_GET_GROUP_INFO = "/v3/qqgroup/get_group_info"
	
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
		path_encode = URI.encode_www_form_component(path)
		parm_encode = URI.encode_www_form_component(parm_encode)
		sig_org = "#{requ_methods}&#{path_encode}&#{parm_encode}";
		sig_org = hamc_sha1(sig_org, APP_KEY + "&");
		return sig_org
	end
	
	#==========================================================================================
	# => 调用API
	#==========================================================================================
	def call_api(open_id, open_key, path, api_parm, method)
		
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


		http = Net::HTTP.new(uri.host, uri.port)
		
		if(method == "POST")
			request = Net::HTTP::Post.new(uri.request_uri)
		elsif (method == "GET")
			request = Net::HTTP::Get.new(uri.request_uri)
		end
		
		request.set_form_data(parm)
		response = http.request(request)

		return response.body
		
	end
	
end

qqapi = QQAPI.new("101096343", "34c6c9b112be0b65fb4f4fae58003c8b")
open_id = "27BC3D2BF826F23EE1EEEB25F0BF1C21"
open_key = "1B98F130305260F2955D3A897955EE57"
gid = "DCEBF31A55AB9E08B2149D4319E6DFFA"

# ret = qqapi.group_send_msg(open_id, open_key, gid, 1, "hi")
ret = qqapi.get_group_info(open_id, open_key, gid)
p ret