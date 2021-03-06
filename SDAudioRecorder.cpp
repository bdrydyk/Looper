
#include "Util.h"
#include "SDAudioRecorder.h"

// inspired by https://github.com/PaulStoffregen/Audio/blob/master/play_sd_raw.cpp

constexpr const char* RECORDING_FILENAME1 = "RECORD1.RAW";
constexpr const char* RECORDING_FILENAME2 = "RECORD2.RAW";


SD_AUDIO_RECORDER::SD_AUDIO_RECORDER() :
  AudioStream(1, m_input_queue_array),
  m_just_played_block(nullptr),
  m_current_play_block(nullptr),
  m_mode(MODE::STOP),
  m_pending_mode(MODE::NONE),
  m_play_back_filename(RECORDING_FILENAME1),
  m_record_filename(RECORDING_FILENAME1),
  m_recorded_audio_file(),
  m_play_back_audio_file(),
  m_play_back_file_size(0),
  m_play_back_file_offset(0),
  m_jump_position(0),
  m_jump_pending(false),
  m_looping(false),
  m_finished_playback(false),
  m_speed(1.0f),
  m_read_head(0.0f),
  m_soft_clip_coefficient(0.0f),
  m_sd_play_queue(*this, "PLAY_QUEUE"),
  m_sd_record_queue(*this, "RECORD_QUEUE")
{
    m_sd_play_queue.start();
}

void SD_AUDIO_RECORDER::update()
{        
  switch( m_mode )
  {
    case MODE::PLAY:
    {
      update_playing_interrupt();

      break; 
    }
    case MODE::RECORD_INITIAL:
    {
      m_sd_record_queue.add_block( create_record_block() );

      break;
    }
    case MODE::RECORD_PLAY:
    case MODE::RECORD_OVERDUB:
    {
      update_playing_interrupt();

      ASSERT_MSG( !m_sd_play_queue.empty(), "Play queue empty, on interrupt" );

      // update after updating play to capture buffer for overdub
      m_sd_record_queue.add_block( create_record_block() );

      break;
    }
    default:
    {
      break;
    }
  }
}

void SD_AUDIO_RECORDER::update_main_loop()
{  
  switch( m_mode )
  {
    case MODE::PLAY:
    {
      if( m_jump_pending )
      {
        if( m_play_back_audio_file.seek( m_jump_position ) )
        {
          m_jump_pending = false;
          m_play_back_file_offset = m_jump_position;
        }
      }

      m_finished_playback = update_playing_sd();

      if( m_finished_playback )
      {       
        if( m_looping )
        {    
          AudioNoInterrupts();
          
          if( m_pending_mode != MODE::NONE )
          {
            ASSERT_MSG( m_pending_mode == MODE::RECORD_PLAY, "Invalid pending mode" );

            start_playing_sd();
            start_recording_sd();

            m_mode          = MODE::RECORD_PLAY;
            m_pending_mode  = MODE::NONE;
          }
          else
          {
            start_playing_sd();
            m_mode = MODE::PLAY;
          }
        
          m_finished_playback = false;
          
          AudioInterrupts();
        }
        else
        {
          m_mode = MODE::STOP;
        }
      }
      break; 
    }
    case MODE::RECORD_INITIAL:
    {
      update_recording_sd();

      break;
    }
    case MODE::RECORD_PLAY:
    case MODE::RECORD_OVERDUB:
    {
      m_finished_playback = update_playing_sd();
      
      update_recording_sd();
      
      // has the loop just finished
      if( m_finished_playback )
      {         
        switch_play_record_buffers();

        AudioNoInterrupts();

        if( m_pending_mode != MODE::NONE )
        {
          ASSERT_MSG( m_pending_mode == MODE::PLAY, "Invalid pending mode" );

          stop_recording_sd();
          start_playing_sd();

          m_mode          = MODE::PLAY;
          m_pending_mode  = MODE::NONE;
        }
        else
        {
          stop_recording_sd();
          start_playing_sd();
          start_recording_sd();
        }

        m_finished_playback = false;
        AudioInterrupts();
      }

      break;
    }
    default:
    {
      break;
    }
  }
}

SD_AUDIO_RECORDER::MODE SD_AUDIO_RECORDER::mode() const
{
  return m_mode;
}
  
void SD_AUDIO_RECORDER::play()
{
  AudioNoInterrupts();

  DEBUG_TEXT_LINE("SD_AUDIO_RECORDER::play()");

  if( m_mode == MODE::RECORD_PLAY || m_mode == MODE::RECORD_OVERDUB )
  {
    m_pending_mode  = MODE::PLAY;
    m_looping       = true;
  }
  else
  {
    play_file( m_play_back_filename, true );
  }

  AudioInterrupts();
}

void SD_AUDIO_RECORDER::play_file( const char* filename, bool loop )
{
  // NOTE - should this be delaying call to start_playing() until update?
  m_play_back_filename = filename;
  m_looping = loop;

  if( m_mode != MODE::PLAY )
  {
    DEBUG_TEXT_LINE("Stop play named file");
    stop_current_mode( false );
  }
  
  if( start_playing_sd() )
  {
    m_mode = MODE::PLAY;
  }
  else
  {
    m_mode = MODE::STOP;
  }
}

void SD_AUDIO_RECORDER::stop()
{
  AudioNoInterrupts();
  
  DEBUG_TEXT("SD_AUDIO_RECORDER::stop() ");
  DEBUG_TEXT_LINE( mode_to_string(m_mode) );
  
  stop_current_mode( true );

  m_mode = MODE::STOP;

  AudioInterrupts();
}

void SD_AUDIO_RECORDER::start_record()
{
  AudioNoInterrupts();
    
  switch( m_mode )
  {
    case MODE::PLAY:
    {
      m_pending_mode       = MODE::RECORD_PLAY;
      
      break;
    }
    case MODE::STOP:
    {
      m_play_back_filename  = RECORDING_FILENAME1;
      m_record_filename     = RECORDING_FILENAME2;

      start_recording_sd();

      m_mode = MODE::RECORD_INITIAL;
      
      break;
    }
    case MODE::RECORD_PLAY:
    {
      m_mode = MODE::RECORD_OVERDUB;
      
      break;
    }
    default:
    {
      DEBUG_TEXT( "SD_AUDIO_RECORDER::start_record() - Invalid mode: " );
      DEBUG_TEXT_LINE( mode_to_string( m_mode ) );
      break;
    }   
  }

  AudioInterrupts();
}

void SD_AUDIO_RECORDER::stop_record()
{
  AudioNoInterrupts();
  
  switch( m_mode )
  {
    case MODE::RECORD_INITIAL:
    {
      stop_recording_sd();

      switch_play_record_buffers();

      start_playing_sd();
      start_recording_sd();

      m_mode = MODE::RECORD_PLAY;

      ASSERT_MSG( !m_sd_play_queue.empty(), "Play queue empty, on finish record initial" );
        
      break;
    }
    case MODE::RECORD_OVERDUB:
    {
      m_mode = MODE::RECORD_PLAY;
      break;      
    }
    default:
    {
      DEBUG_TEXT( "SD_AUDIO_RECORDER::start_record() - Invalid mode: " );
      DEBUG_TEXT_LINE( mode_to_string( m_mode ) );
      break;
    }   
  }

  AudioInterrupts();
}

bool SD_AUDIO_RECORDER::mode_pending() const
{
  return m_pending_mode != MODE::NONE;
}

void SD_AUDIO_RECORDER::set_read_position( float t )
{
 if( m_mode == MODE::PLAY )
 {
  const uint32_t block_size   = 2; // AUDIO_BLOCK_SAMPLES
  const uint32_t file_pos     = m_play_back_file_size * t;
  const uint32_t block_rem    = file_pos % block_size;
  
  
  m_jump_pending  = true;
  m_jump_position = file_pos + block_rem;
 }
}

audio_block_t* SD_AUDIO_RECORDER::create_record_block()
{
  // if overdubbing, add incoming audio, otherwise re-record the original audio
  if( m_mode == MODE::RECORD_PLAY )
  {
      ASSERT_MSG( m_just_played_block != nullptr, "Cannot record play, no block" ); // can it be null if overdub exceeds original play file?  

      audio_block_t* play_block = m_just_played_block;
      m_just_played_block = nullptr;

      return play_block;
  }
  if( m_mode == MODE::RECORD_OVERDUB )
  {
    ASSERT_MSG( m_just_played_block != nullptr, "Cannot overdub, no just_played_block" ); // can it be null if overdub exceeds original play file?
    audio_block_t* in_block = receiveWritable();
    ASSERT_MSG( in_block != nullptr, "Overdub - unable to receive block" );

    // mix incoming audio with recorded audio ( from update_playing() ) then release
    if( in_block != nullptr && m_just_played_block != nullptr )
    {
      for( int i = 0; i < AUDIO_BLOCK_SAMPLES; ++i )
      {
        const int32_t summed_sample = in_block->data[i] + m_just_played_block->data[i]; // need to add them in 32 bits to avoid wrap-around
        const int16_t sample16      = clamp<int32_t>( summed_sample, std::numeric_limits<int16_t>::lowest(), std::numeric_limits<int16_t>::max() );
        in_block->data[i]           = soft_clip_sample( sample16 );
        ASSERT_MSG( in_block->data[i] < std::numeric_limits<int16_t>::max() && in_block->data[i] > std::numeric_limits<int16_t>::min(), "CLIPPING" );
      }
    }
    else
    {
      //ASSERT_MSG( in_block != nullptr, "SD_AUDIO_RECORDER::aquire_block_func() no in_block" );
      ASSERT_MSG( m_just_played_block != nullptr, "SD_AUDIO_RECORDER::aquire_block_func() no just_played_block" );

      audio_block_t* play_block = m_just_played_block;
      m_just_played_block = nullptr;

      return play_block;
    }

    if( m_just_played_block != nullptr )
    {
      release( m_just_played_block );
      m_just_played_block = nullptr;
    }

    return in_block;
  }
  else
  {
    ASSERT_MSG( m_mode == MODE::RECORD_INITIAL, "What mode is this?" );
    audio_block_t* in_block = receiveReadOnly();
    ASSERT_MSG( in_block != nullptr, "Record Initial - unable to receive block" );

    return in_block;
  }
}

void SD_AUDIO_RECORDER::release_block_func(audio_block_t* block)
{
  release(block);
}

bool SD_AUDIO_RECORDER::start_playing_sd()
{
  DEBUG_TEXT("SD_AUDIO_RECORDER::start_playing_sd() ");
  DEBUG_TEXT_LINE( m_play_back_filename );

  m_read_head = 0.0f;
  
  stop_playing_sd();

  ASSERT_MSG( m_current_play_block == nullptr, "Leaking current play block" );

  enable_SPI_audio();
  __disable_irq();
  m_play_back_audio_file = SD.open( m_play_back_filename );
  __enable_irq();
  
  if( !m_play_back_audio_file )
  {
    DEBUG_TEXT("Unable to open file: ");
    DEBUG_TEXT_LINE( m_play_back_filename );
#if defined(HAS_KINETIS_SDHC)
      if (!(SIM_SCGC3 & SIM_SCGC3_SDHC)) AudioStopUsingSPI();
#else
      AudioStopUsingSPI();
#endif

    return false;
  }

  DEBUG_TEXT("Play File loaded ");
  DEBUG_TEXT(m_play_back_filename);
  m_play_back_file_size = m_play_back_audio_file.size();
  m_play_back_file_offset = 0;
  DEBUG_TEXT(" file size: ");
  DEBUG_TEXT_LINE(m_play_back_file_size);

  // prime the first read block in the read queue
  for( int i = 0; i < INITIAL_PLAY_BLOCKS; ++i )
  {
    update_playing_sd();
  }

  return true;
}

bool SD_AUDIO_RECORDER::update_playing_sd()
{
  bool finished = false;

  if( m_play_back_audio_file.available() )
  {    
    if( m_sd_play_queue.remaining() > 0 &&
        (m_mode != MODE::PLAY || m_sd_play_queue.size() <= MAX_PREFERRED_RECORD_BLOCKS_WHEN_PLAYING) )
    {
      // allocate the audio blocks to transmit
      audio_block_t* block = allocate();
      if( block == nullptr )
      {
        DEBUG_TEXT_LINE( "update_playing_sd() - Failed to allocate" );
        return false;
      }
    
      // we can read more data from the file...
      uint32_t n = 0;
      {
        ADD_TIMED_SECTION( "Read time", 2500 );
        n = m_play_back_audio_file.read( block->data, AUDIO_BLOCK_SAMPLES*2 );
      }

      m_play_back_file_offset += n;
      for( int i = n/2; i < AUDIO_BLOCK_SAMPLES; i++ )
      {
        block->data[i] = 0;
      }

      if( block == nullptr )
      {
        DEBUG_TEXT_LINE("Adding null play block");
      }
      m_sd_play_queue.add_block( block );
    }
  }
  else
  {
    DEBUG_TEXT("File End ");
    DEBUG_TEXT_LINE(m_play_back_audio_file.name());

    disable_SPI_audio();
            
    finished = true;
  }

  return finished;
}

void SD_AUDIO_RECORDER::update_playing_interrupt()
{  
  if( m_sd_play_queue.size() > 0 )
  {
    // when recording - speed is always 1 and need to set just_played_block for overdub
    if( is_recording() )
    {
      audio_block_t* block = m_sd_play_queue.read_block();
      ASSERT_MSG( block != nullptr, "update_playing_interrupt() null block" );
      transmit( block );  
      m_sd_play_queue.release_buffer(false);

      ASSERT_MSG( m_just_played_block == nullptr, "Leaking just_played_block" );
      m_just_played_block = block;
    }
    // when playing - apply speed to audio playback
    else
    {
      audio_block_t* block_to_transmit = allocate();

      if( block_to_transmit == nullptr )
      {
        DEBUG_TEXT_LINE( "Unable to allocate block_to_transmit" );
        return;
      }

      auto get_next_play_block = [this]()
      {       
        audio_block_t* play_block = m_sd_play_queue.read_block();
        ASSERT_MSG( play_block != nullptr, "update_playing_interrupt() get_next_play_block() null block" );
        m_sd_play_queue.release_buffer(false);

        return play_block;
      };

      auto read_from_block_with_speed = []( const audio_block_t* source, audio_block_t* target, float speed, float& read_head, int& write_head )
      {
        while( static_cast<int>(read_head) < AUDIO_BLOCK_SAMPLES && write_head < AUDIO_BLOCK_SAMPLES )
        {
          //target->data[write_head] = source->data[static_cast<int>(read_head)]; // no interpolation
          target->data[write_head] = DSP_UTILS::read_sample_cubic( read_head, source->data, AUDIO_BLOCK_SAMPLES );
          ++write_head;
          read_head += speed;
        }
      };
      
      if( m_current_play_block == nullptr || static_cast<int>(m_read_head) >= AUDIO_BLOCK_SAMPLES )
      {
        m_read_head           = 0.0f;
        m_current_play_block  = get_next_play_block();
      }

      int write_head = 0;
      while( write_head < AUDIO_BLOCK_SAMPLES )
      {
        // read from current play block
        read_from_block_with_speed( m_current_play_block, block_to_transmit, m_speed, m_read_head, write_head );
        if( static_cast<int>(m_read_head) >= AUDIO_BLOCK_SAMPLES )
        {
          // end of block reached - fetch another block from the queue
          release( m_current_play_block );
          m_read_head           = 0.0f;
          m_current_play_block  = get_next_play_block();
        }
      }

      transmit( block_to_transmit );

      release( block_to_transmit );
    }
  }
  else
  {
    ASSERT_MSG( m_finished_playback, "PLAY QUEUE EMPTY!!" );
  }
}

void SD_AUDIO_RECORDER::stop_playing_sd()
{
  DEBUG_TEXT_LINE("SD_AUDIO_RECORDER::stop_playing_sd");

  __disable_irq();
  if( m_mode == MODE::PLAY || m_mode == MODE::RECORD_PLAY || m_mode == MODE::RECORD_OVERDUB )
  {    
    m_play_back_audio_file.close();
    
    disable_SPI_audio();
  }
  __enable_irq();

  if( m_current_play_block != nullptr )
  {
    release( m_current_play_block );
    m_current_play_block = nullptr;
  }

  // TODO - do we need to write the rest of the queue?
  //m_sd_play_queue.stop();
}

void SD_AUDIO_RECORDER::start_recording_sd()
{  
  DEBUG_TEXT("SD_AUDIO_RECORDER::start_recording_sd() ");
  DEBUG_TEXT_LINE(m_record_filename);
  if( SD.exists( m_record_filename ) )
  {
    // delete previously existing file (SD library will append to the end)
    SD.remove( m_record_filename ); 
  } 
  
  m_recorded_audio_file = SD.open( m_record_filename, FILE_WRITE );

  if( m_recorded_audio_file )
  {
    m_sd_record_queue.start();
    DEBUG_TEXT_LINE( m_record_filename );
  }
  else
  {
    DEBUG_TEXT("Unable to open file: ");
    DEBUG_TEXT_LINE( m_record_filename );
  }
}

void SD_AUDIO_RECORDER::update_recording_sd()
{
  // Simple balancing system to keep play queue from emptying whilst preventing record queue from getting full
  const int record_queue_size = m_sd_record_queue.size(); 
  if( record_queue_size >= 2 && 
      ( m_mode == MODE::RECORD_INITIAL || m_sd_play_queue.size() >= MIN_PREFERRED_PLAY_BLOCKS || record_queue_size >= MAX_PREFERRED_RECORD_BLOCKS ) )
  {
    byte buffer[512]; // arduino library most efficient with full 512 sector size writes

    // write 2 x 256 byte blocks to buffer
    memcpy( buffer, m_sd_record_queue.read_buffer(), 256);
    m_sd_record_queue.release_buffer();
    memcpy( buffer + 256, m_sd_record_queue.read_buffer(), 256);
    m_sd_record_queue.release_buffer();

    // soft clip the buffer
    for( int s = 0; s < 512; ++s )
    {
      buffer[s] = soft_clip_sample( buffer[s] );
    }

    ADD_TIMED_SECTION( "Write time", 8000 );
    m_recorded_audio_file.write( buffer, 512 );
  }
}

void SD_AUDIO_RECORDER::stop_recording_sd( bool write_remaining_blocks )
{
  DEBUG_TEXT_LINE("SD_AUDIO_RECORDER::stop_recording_sd()");
  m_sd_record_queue.stop();

  if( is_recording() )
  {
    if( m_just_played_block != nullptr )
    {
      release( m_just_played_block );
      m_just_played_block = nullptr;
    }
    
    // empty the record queue
    if( write_remaining_blocks )
    {
      DEBUG_TEXT("Writing final blocks:");
      DEBUG_TEXT_LINE( m_sd_record_queue.size() );
      while( m_sd_record_queue.size() > 0 )
      {
        m_recorded_audio_file.write( reinterpret_cast<byte*>(m_sd_record_queue.read_buffer()), 256 );
        m_sd_record_queue.release_buffer();
      }
    }
    else
    {
      m_sd_record_queue.clear();
    }

    m_recorded_audio_file.close();
  }
}

void SD_AUDIO_RECORDER::stop_current_mode( bool reset_play_file )
{ 
  switch( m_mode )
  {
    case MODE::PLAY:
    {
      stop_playing_sd();
      break; 
    }
    case MODE::RECORD_INITIAL:
    {
      stop_recording_sd();
      break;
    }
    case MODE::RECORD_PLAY:
    case MODE::RECORD_OVERDUB:
    {
      stop_playing_sd();
      stop_recording_sd();

      m_sd_play_queue.clear();
      m_sd_record_queue.clear();

      ASSERT_MSG( m_just_played_block == nullptr, "This should have been reset in stop_recording_sd()" );
      
      break;
    }
    default:
    {
      break;
    }
  }

  if( reset_play_file )
  {
    m_play_back_filename = m_record_filename = RECORDING_FILENAME1;
  }
}

void SD_AUDIO_RECORDER::switch_play_record_buffers()
{
  // toggle record/play filenames
  swap( m_play_back_filename, m_record_filename );

  //DEBUG_TEXT( "switch_play_record_buffers() Play: ");
  //DEBUG_TEXT( m_play_back_filename );
  //DEBUG_TEXT(" Record: " );
  //DEBUG_TEXT_LINE( m_record_filename );
}

int16_t SD_AUDIO_RECORDER::soft_clip_sample( int16_t sample ) const
{
  return DSP_UTILS::soft_clip_sample( sample, m_soft_clip_coefficient );
}

void SD_AUDIO_RECORDER::set_saturation( float saturation )
{
  constexpr const float MIN_SATURATION = 0.0f;
  constexpr const float MAX_SATURATION = 1.0f / 3.0f;

  m_soft_clip_coefficient = lerp( MIN_SATURATION, MAX_SATURATION, saturation );
}

void SD_AUDIO_RECORDER::set_speed( float speed )
{
  constexpr const float MIN_SPEED = 0.25f;
  constexpr const float MAX_SPEED = 2.0f;

  m_speed = lerp( MIN_SPEED, MAX_SPEED, speed ); 
}

const char* SD_AUDIO_RECORDER::mode_to_string( MODE mode )
{
  switch( mode )
  {
    case MODE::PLAY:
    {
      return "PLAY";
    }
    case MODE::STOP:
    {
      return "STOP";
    }
    case MODE::RECORD_INITIAL:
    {
      return "RECORD_INITIAL";
    }
    case MODE::RECORD_PLAY:
    {
      return "RECORD_PLAY";
    }
    case MODE::RECORD_OVERDUB:
    {
      return "RECORD_OVERDUB";
    }
    default:
    {
      return nullptr;
    }
  }
}

uint32_t SD_AUDIO_RECORDER::play_back_file_time_ms() const
{
  const uint64_t num_samples = m_play_back_file_size / 2;
  const uint64_t time_in_ms = ( num_samples * 1000 ) / AUDIO_SAMPLE_RATE;

  //DEBUG_TEXT("Play back time in seconds:");
  //DEBUG_TEXT_LINE(time_in_ms / 1000.0f);

  return time_in_ms;
}

float SD_AUDIO_RECORDER::playback_position() const
{
  if( m_play_back_file_size > 0 )
  {
    return m_play_back_file_offset / static_cast<float>(m_play_back_file_size);
  }
  else
  {
    // no file recorded
    return 0;
  }
}
