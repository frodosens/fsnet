require 'fiddle/import'
require 'json'

module FSNet
  module Native
    extend Fiddle::Importer
    dlload ENV.fetch('FSNET_LIBRARY', File.expand_path('../../../build/libfsnet.so', __FILE__))
    extern 'const char* fsnet_last_error()'
    extern 'void* fsnet_create(const char*)'
    extern 'void fsnet_destroy(void*)'
    extern 'int fsnet_listen(void*, const char*, int)'
    extern 'long long fsnet_connect(void*, const char*, int)'
    extern 'int fsnet_bind(void*, const char*, void*)'
    extern 'int fsnet_respond(void*, unsigned long long, int, const char*, size_t)'
    extern 'int fsnet_call(void*, unsigned long long, const char*, const char*, size_t, int, void*)'
    extern 'int fsnet_poll(void*)'
    extern 'void* fsnet_db_open(const char*)'
    extern 'void fsnet_db_close(void*)'
    extern 'void* fsnet_db_get(void*, const char*, size_t, void*, void*)'
    extern 'int fsnet_db_cas(void*, const char*, size_t, unsigned long long, const char*, size_t)'
    extern 'int fsnet_db_erase(void*, const char*, size_t, unsigned long long)'
    extern 'int fsnet_db_fenced_cas(void*, const char*, size_t, unsigned long long, const char*, size_t, unsigned long long, const char*, size_t)'
    extern 'void fsnet_free(void*)'

    def self.check(value)
      raise Error, fsnet_last_error.to_s if value == -1 || (value.respond_to?(:null?) && value.null?)
      value
    end
  end
  class Error < StandardError; end
  class RPCError < Error
    attr_reader :status
    def initialize(status)
      @status = status
      super("RPC failed: #{%w[ok not_found overloaded timeout disconnected rejected internal].fetch(status, status.to_s)}")
    end
  end

  class Database
    Record = Struct.new(:value, :version, keyword_init: true)
    def initialize(path: '')
      @handle = Native.check(Native.fsnet_db_open(path))
    end
    def close
      Native.fsnet_db_close(@handle) if @handle
      @handle = nil
    end
    def get(key)
      ensure_open
      key = String(key)
      length = Fiddle::Pointer.malloc(Fiddle::SIZEOF_SIZE_T)
      version = Fiddle::Pointer.malloc(8)
      data = Native.fsnet_db_get(@handle, key, key.bytesize, length, version)
      if data.null?
        message = Native.fsnet_last_error.to_s
        raise Error, message unless message.empty?
        return nil
      end
      begin
        Record.new(value: JSON.parse(data.to_s(length[0, Fiddle::SIZEOF_SIZE_T].unpack1('J'))),
                   version: version[0, 8].unpack1('Q'))
      ensure
        Native.fsnet_free(data)
      end
    end
    def save(key, value, version: 0)
      ensure_open
      raise ArgumentError, 'invalid version' unless version.is_a?(Integer) && version >= 0 && version < 2**63 - 1
      key = String(key)
      data = JSON.generate(value)
      Native.check(Native.fsnet_db_cas(@handle, key, key.bytesize, version, data, data.bytesize)) == 1
    end
    def delete(key, version:)
      ensure_open
      raise ArgumentError, 'invalid version' unless version.is_a?(Integer) && version >= 0 && version < 2**63
      key = String(key)
      Native.check(Native.fsnet_db_erase(@handle, key, key.bytesize, version)) == 1
    end
    def save_fenced(key, value, version:, guard_key:, fence:)
      ensure_open
      [version, fence].each do |number|
        raise ArgumentError, 'invalid version' unless number.is_a?(Integer) && number >= 0 && number < 2**63 - 1
      end
      key, guard_key = String(key), String(guard_key)
      data = JSON.generate(value)
      Native.check(Native.fsnet_db_fenced_cas(@handle, guard_key, guard_key.bytesize, fence,
        key, key.bytesize, version, data, data.bytesize)) == 1
    end
    private
    def ensure_open
      raise Error, 'database closed' unless @handle
    end
  end

  class EntityStore
    def initialize(database)
      @database = database
    end
    def owner(id)
      @database.get("fsnet/owner/#{id}")
    end
    def transfer(id, owner, expected_fence: 0)
      raise ArgumentError, 'entity and owner required' if id.to_s.empty? || owner.to_s.empty?
      @database.save("fsnet/owner/#{id}", owner, version: expected_fence)
    end
    def load(id)
      @database.get("fsnet/state/#{id}")
    end
    def save(id, value, fence:, version: 0)
      @database.save_fenced("fsnet/state/#{id}", value, version: version,
                           guard_key: "fsnet/owner/#{id}", fence: fence)
    end
  end

  class Node
    def initialize(token:)
      @handle = Native.check(Native.fsnet_create(token))
      @handlers = []
      @fibers = {}
      @pending = {}
      @sequence = 0
    end
    def listen(host: '127.0.0.1', port: 0)
      ensure_open
      Native.check(Native.fsnet_listen(@handle, host, port))
    end
    def connect(host:, port:)
      ensure_open
      Native.check(Native.fsnet_connect(@handle, host, port))
    end
    def rpc(route, &handler)
      ensure_open
      raise ArgumentError, 'handler required' unless handler
      closure = Fiddle::Closure::BlockCaller.new(Fiddle::TYPE_VOID,
        [Fiddle::TYPE_LONG_LONG, Fiddle::TYPE_VOIDP, Fiddle::TYPE_SIZE_T]) do |id, bytes, length|
        # Copy native memory before suspension: the packet buffer is ephemeral.
        payload = Fiddle::Pointer.new(bytes).to_s(length)
        fiber = Fiber.new do
          begin
            data = JSON.parse(payload)
            result = JSON.generate(handler.call(data))
            Native.fsnet_respond(@handle, id, 0, result, result.bytesize)
          rescue StandardError => error
            warn "fsnet #{route}: #{error.class}: #{error.message}"
            Native.fsnet_respond(@handle, id, 6, '', 0) if @handle
          ensure
            @fibers.delete(id)
          end
        end
        @fibers[id] = fiber
        fiber.resume
      end
      Native.check(Native.fsnet_bind(@handle, route, closure))
      @handlers << closure # Native callbacks must stay rooted across GC.
      self
    end
    def call(peer, route, params = {}, timeout: 1000, &completion)
      ensure_open
      raise ArgumentError, 'completion required' unless completion
      data = JSON.generate(params)
      id = (@sequence += 1)
      closure = Fiddle::Closure::BlockCaller.new(Fiddle::TYPE_VOID,
        [Fiddle::TYPE_INT, Fiddle::TYPE_VOIDP, Fiddle::TYPE_SIZE_T]) do |status, bytes, length|
        begin
          result = status == 0 ? JSON.parse(Fiddle::Pointer.new(bytes).to_s(length)) : nil
          completion.call(result, status == 0 ? nil : RPCError.new(status))
        rescue StandardError => error
          warn "fsnet completion: #{error.class}: #{error.message}"
        ensure
          @pending.delete(id)
        end
      end
      @pending[id] = closure
      Native.check(Native.fsnet_call(@handle, peer, route, data, data.bytesize, timeout, closure))
      self
    rescue StandardError
      @pending.delete(id) if id
      raise
    end
    # Sequential-looking RPC inside a service fiber; the owning event loop keeps
    # processing other requests while this fiber waits, with the same deadline.
    def request(peer, route, params = {}, timeout: 1000)
      fiber = Fiber.current
      raise Error, 'request must run inside an RPC handler' unless @fibers.value?(fiber)
      completed = suspended = false
      outcome = nil
      call(peer, route, params, timeout: timeout) do |value, failure|
        outcome = [value, failure]
        completed = true
        fiber.resume if suspended && fiber.alive?
      end
      unless completed
        suspended = true
        Fiber.yield
      end
      raise outcome[1] if outcome[1]
      outcome[0]
    end
    def mount(name, service)
      service.class.routes.each do |route, handler|
        rpc("#{name}.#{route}") { |params| service.instance_exec(params, &handler) }
      end
      self
    end
    def poll
      ensure_open
      raise Error, 'recursive poll is not supported' if @polling
      @polling = true
      begin
        Native.check(Native.fsnet_poll(@handle))
      ensure
        @polling = false
        close if @closing
      end
    end
    def run
      loop { poll; sleep 0.001 }
    end
    def close
      if @polling
        @closing = true
        return
      end
      Native.fsnet_destroy(@handle) if @handle
      @handle = nil
      @pending.clear
      @handlers.clear
      @fibers.clear
    end
    private
    def ensure_open
      raise Error, 'node closed' unless @handle
    end
  end

  class Service
    def self.routes
      @routes ||= {}
    end
    def self.rpc(name, &handler)
      raise ArgumentError, 'handler required' unless handler
      raise ArgumentError, 'duplicate route' if routes.key?(name.to_s)
      routes[name.to_s] = handler
    end
    def self.inherited(child)
      child.instance_variable_set(:@routes, routes.dup)
    end
  end
end
