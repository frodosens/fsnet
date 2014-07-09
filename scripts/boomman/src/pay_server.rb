

require 'http_server.rb'
class PayServer < HTTPServer
	
	def on_handle_request(request)
    
    response = HTTPResponse.new(200, request.params.to_s);
		
		
		
		return response
		
	end
	
	
	
end