
require 'logger'

class FSLogger


	def initialize()

		@std_logger = Logger.new($stdout)
		@file_logger = Logger.new('game.log')

	end

	def set_tag tag
		@tag = tag
	end

	def info(msg, *args)
		@std_logger.info(@tag) { format(msg, *args) }
		@file_logger.info(@tag) { format(msg, *args) }
	end

	def warn(msg, *args)
		@std_logger.warn(@tag) { format(msg, *args) }
		@file_logger.warn(@tag) { format(msg, *args) }
	end

	def error(msg, *args)
		@std_logger.error(@tag) { format(msg, *args) }
		@file_logger.error(@tag) { format(msg, *args) }
	end
	class << self

		@@logger = FSLogger.new

		def get_logger(tag)

			@@logger.set_tag tag.class.name

			@@logger

		end


	end

end