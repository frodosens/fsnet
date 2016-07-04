require 'rubylib/tcp_pack'

require 'dist_server/util/hash'
require 'dist_server/util/bool'
require 'dist_server/util/array'
require 'dist_server/util/nil_class'
require 'dist_server/util/uuid'

class Message


	attr_reader :header
	attr_reader :method
	attr_reader :params


	def initialize(h, m, par)
		@header = h
		@method = m
		@params = par
	end

	def set_callback_id(id)
		@header['callback_id'] = id
	end
	def get_callback_id
		return @header['callback_id']
	end

	def need_return?
		@header.has_key?('callback_id')
	end

	def is_return_package?
		@header.has_key?('is_return_package')
	end

	def get_return_params
		return @params[0]
	end

	def to_fs_pack()

		fs_pack = FSPack.new
		fs_pack.write_data.write_hash(@header)
		fs_pack.write_data.write_string(@method)
		fs_pack.write_data.write_params_array(@params)
		fs_pack

	end

	class << self

		def create(header, method, params)

			Message.new(header, method, params)

		end

		def create_return_pack(from_message, ret)
			from_message.header['is_return_package'] = true
			Message.new(from_message.header, '', [ret])

		end

		def create_from_pack(pack)

			header = pack.read_data.read_hash
			method = pack.read_data.read_string
			params = pack.read_data.read_params_array

			Message.new(header, method, params)

		end


	end

end