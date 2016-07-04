module RPCDefine

		def define_rpc(method_name)


			class_eval("

				def #{method_name}_async(*arg)
					msg = Message.create({}, '#{method_name}', arg)
					send_message_async(msg)
        end

        def #{method_name}_sync(*arg)
					msg = Message.create({}, '#{method_name}', arg)
					send_message_sync(msg)
        end

        def #{method_name}(*arg)
					 #{method_name}_async(*arg)
				end
        ", __FILE__, __LINE__)

		end

end