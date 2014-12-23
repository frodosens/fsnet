module EntitySystem



	attr_reader :entities

	def init_entities
		@entities = {}
	end

	def find_entity(key, operation={ :with => :klass })
		if operation[:with] == :uuid
			return @entities[key]
		end

		if operation[:with] == :klass
			for k, v in @entities
				if v.class == key
					return v
				end
			end
			nil
		end
		nil
	end

	def create_entity(klass, remote_klass_name=klass.to_s, entity_data=nil, entity_id=nil)

		entity = klass.create(entity_id)
		entity.remote_klass_name = remote_klass_name
		entity.owner = self
		entity.on_create(entity_data)
		@entities[entity.uuid] = entity

	end

	def destroy_entity(uuid)

		entity = @entities[uuid]
		if entity.nil?
			self.server.warn("try destroy entity fail : #{uuid}")
			return
		end

		entity.on_destroy
		@entities.delete(uuid)

	end

	def message_entity( uuid, method_name, params, call_serial=0 )


		entity = @entities[uuid]
		if entity.nil?
			self.server.warn("try call message #{method_name} entity fail : #{uuid}")
			return
		end
		entity.calling_serial = call_serial

		method = entity.method(method_name)
		method.call(*params)

		entity.calling_serial = -1
	end




end