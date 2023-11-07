#include "data_tamer/channel.hpp"
#include "data_tamer/data_sink.hpp"
#include "data_tamer/contrib/SerializeMe.hpp"

#include <iostream>

namespace DataTamer
{

RegistrationID LogChannel::registerValueImpl(
    const std::string& name, ValuePtr&& value_ptr)
{
  if (name.find(' ') != std::string::npos)
  {
    throw std::runtime_error("name can not contain spaces");
  }

  std::lock_guard const lock(mutex_);
  active_flags_dirty_ = true;

  // check if this name exists already
  auto it = registered_values_.find(name);
  if( it == registered_values_.end())
  {
    // appending a new ValueHolder to series
    ValueHolder instance;
    instance.name = name;
    instance.holder = std::move(value_ptr);
    series_.emplace_back(std::move(instance));
    const auto type = value_ptr.type();

    const size_t index = series_.size() -1;
    registered_values_.insert( {name, index} );
    payload_buffer_size_ += SizeOf(type);

    // update schema and its hash (append only)
    // https://stackoverflow.com/questions/2590677/how-do-i-combine-hash-values-in-c0x
    std::hash<std::string> str_hasher;
    std::hash<BasicType> type_hasher;

    schema_.fields.emplace_back( Schema::Field{name, type} );

    auto& hash = schema_.hash;
    hash ^= str_hasher(name) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    hash ^= type_hasher(type) + 0x9e3779b9 + (hash << 6) + (hash >> 2);

    return {index, 1};
  }

  // trying to registered again an unregistered holder
  const size_t index = it->second;
  auto& instance = series_[index];

  if(instance.registered)
  {
    throw std::runtime_error("Value already registered");
  }
  const auto old_type = instance.holder.type();
  const auto new_type = value_ptr.type();
  if(new_type != old_type)
  {
    throw std::runtime_error("Can't change the type of a previously "
                             "unregistered value");
  }
  // mark as registered again
  instance.registered = true;
  instance.enabled = true;
  payload_buffer_size_ += SizeOf(new_type);
  return {index, 1};
}

LogChannel::LogChannel(std::string name) : channel_name_(name)
{
  std::hash<std::string> str_hasher;
  schema_.hash = str_hasher(channel_name_);
}

LogChannel::~LogChannel()
{
}

void LogChannel::setEnabled(const RegistrationID& id, bool enable)
{
  std::lock_guard const lock(mutex_);
  for(size_t i=0; i<id.fields_count; i++)
  {
    auto& instance = series_[id.first_index + i];
    if(instance.enabled != enable)
    {
      instance.enabled = enable;
      // if changed, adjust the size of the payload
      const auto size = SizeOf(instance.holder.type());
      payload_buffer_size_ += enable ? size : -size;
    }
  }
}

void LogChannel::unregister(const RegistrationID& id)
{
  std::lock_guard const lock(mutex_);
  for(size_t i=0; i<id.fields_count; i++)
  {
    auto& instance = series_[id.first_index + i];
    instance.registered = false;
    instance.enabled = false;
    payload_buffer_size_ -= SizeOf(instance.holder.type());
  }
}

void LogChannel::addDataSink(std::shared_ptr<DataSinkBase> sink)
{
  sinks_.insert(sink);
}

void LogChannel::updateActiveMask()
{
  if (active_flags_dirty_)
  {
    active_flags_dirty_ = false;
    active_flags_.clear();
    active_flags_.resize((series_.size() + 7) / 8, 0xFF);  // ceiling
    for (size_t i = 0; i < series_.size(); i++)
    {
      auto const& instance = series_[i];
      if (!instance.enabled)
      {
        SetBit(active_flags_, i, false);
      }
    }
  }
  if( schema_.hash != prev_schema_hash_)
  {
    prev_schema_hash_ = schema_.hash;
    for(auto const& sink: sinks_)
    {
      sink->addChannel(channel_name_, schema_);
    }
  }
}

Schema LogChannel::getSchema() const
{
  std::lock_guard const lock(mutex_);
  return schema_;
}

bool LogChannel::takeSnapshot(std::chrono::nanoseconds timestamp)
{
  {
    std::lock_guard const lock(mutex_);
    if(sinks_.empty())
    {
      return false;
    }
    updateActiveMask();
    // serialize
    size_t total_size = sizeof(uint64_t) + // timestamp
                        sizeof(uint64_t) + // schema hash
                        sizeof(uint32_t) + // payload size
                        SerializeMe::BufferSize(active_flags_) +
                        payload_buffer_size_;

    snapshot_buffer_.resize(total_size);

    SerializeMe::SpanBytes write_view(snapshot_buffer_);
    SerializeIntoBuffer( write_view, schema_.hash );
    SerializeIntoBuffer( write_view, timestamp.count() );
    SerializeIntoBuffer( write_view, active_flags_ );

    for(auto const& entry: series_)
    {
      if(entry.enabled)
      {
        const auto size = entry.holder.serialize(write_view.data());
        write_view.trimFront(size);
      }
    }
    for(auto& sink: sinks_)
    {
      sink->pushSnapshot(snapshot_buffer_);
    }
  }
  return true;
}


} // namespace DataTamer
