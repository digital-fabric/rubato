# frozen_string_literal: true

require 'modulation'

Nuclear = import('../../lib/nuclear')

spawn do
  puts "going to sleep..."
  cancel_after(1) do
    await async do
      await sleep 2
    end
  end
rescue Nuclear::Cancelled => e
  puts "got error: #{e}"
ensure
  puts "woke up"
end

