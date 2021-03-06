# frozen_string_literal: true

require 'openssl'
require_relative './socket'

# OpenSSL socket helper methods (to make it compatible with Socket API) and overrides
class ::OpenSSL::SSL::SSLSocket
  alias_method :orig_initialize, :initialize
  def initialize(socket, context = nil)
    socket = socket.respond_to?(:io) ? socket.io || socket : socket
    context ? orig_initialize(socket, context) : orig_initialize(socket)
  end

  def dont_linger
    io.dont_linger
  end

  def no_delay
    io.no_delay
  end

  def reuse_addr
    io.reuse_addr
  end

  def fill_rbuff
    data = self.sysread(BLOCK_SIZE)
    if data
      @rbuffer << data
    else
      @eof = true
    end
  end

  alias_method :orig_sysread, :sysread
  def sysread(maxlen, buf = +'')
    while true
      case (result = read_nonblock(maxlen, buf, exception: false))
      when :wait_readable then Polyphony.backend_wait_io(io, false)
      when :wait_writable then Polyphony.backend_wait_io(io, true)
      else return result
      end
    end
  end

  alias_method :orig_syswrite, :syswrite
  def syswrite(buf)
    while true
      case (result = write_nonblock(buf, exception: false))
      when :wait_readable then Polyphony.backend_wait_io(io, false)
      when :wait_writable then Polyphony.backend_wait_io(io, true)
      else
        return result
      end
    end
  end

  def flush
    # osync = @sync
    # @sync = true
    # do_write ""
    # return self
    # ensure
    # @sync = osync
  end

  def readpartial(maxlen, buf = +'', buffer_pos = 0)
    if buffer_pos != 0
      if (result = sysread(maxlen, +''))
        if buffer_pos == -1
          result = buf + result
        else
          result = buf[0...buffer_pos] + result
        end
      end
    else
      result = sysread(maxlen, buf)
    end
    result || (raise EOFError)
  end

  def read_loop(maxlen = 8192)
    while (data = sysread(maxlen))
      yield data
    end
  end
  alias_method :recv_loop, :read_loop

  alias_method :orig_peeraddr, :peeraddr
  def peeraddr(_ = nil)
    orig_peeraddr
  end
end

# OpenSSL socket helper methods (to make it compatible with Socket API) and overrides
class ::OpenSSL::SSL::SSLServer
  def accept_loop(ignore_errors = true)
    loop do
      yield accept
    rescue SystemCallError, StandardError => e
      raise e unless ignore_errors
    end
  end
end
