/*
 * song.cpp - root of the model tree
 *
 * Copyright (c) 2004-2014 Tobias Doerffel <tobydox/at/users.sourceforge.net>
 *
 * This file is part of LMMS - http://lmms.io
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program (see COPYING); if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301 USA.
 *
 */

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QApplication>

#include <math.h>

#include "Song.h"
#include "AutomationTrack.h"
#include "AutomationEditor.h"
#include "BBEditor.h"
#include "BBTrack.h"
#include "BBTrackContainer.h"
#include "ConfigManager.h"
#include "ControllerRackView.h"
#include "ControllerConnection.h"
#include "embed.h"
#include "EnvelopeAndLfoParameters.h"
#include "ExportProjectDialog.h"
#include "FxMixer.h"
#include "FxMixerView.h"
#include "ImportFilter.h"
#include "InstrumentTrack.h"
#include "MainWindow.h"
#include "FileDialog.h"
#include "MidiClient.h"
#include "DataFile.h"
#include "NotePlayHandle.h"
#include "Pattern.h"
#include "PianoRoll.h"
#include "ProjectJournal.h"
#include "ProjectNotes.h"
#include "ProjectRenderer.h"
#include "RenameDialog.h"
#include "SongEditor.h"
#include "templates.h"
#include "TextFloat.h"
#include "Timeline.h"
#include "PeakController.h"


tick_t MidiTime::s_ticksPerTact = DefaultTicksPerTact;



Song::Song() :
	TrackContainer(),
	m_tempoModel( DefaultTempo, MinTempo, MaxTempo, this, tr( "Tempo" ) ),
	m_timeSigModel( this ),
	m_oldTicksPerTact( DefaultTicksPerTact ),
	m_masterVolumeModel( 100, 0, 200, this, tr( "Master volume" ) ),
	m_masterPitchModel( 0, -12, 12, this, tr( "Master pitch" ) ),
	m_fileName(),
	m_oldFileName(),
	m_modified( false ),
	m_recording( false ),
	m_exporting( false ),
	m_exportLoop( false ),
	m_playing( false ),
	m_paused( false ),
	m_loadingProject( false ),
	m_playMode( Mode_None ),
	m_length( 0 ),
	m_trackToPlay( NULL ),
	m_patternToPlay( NULL ),
	m_loopPattern( false ),
	m_elapsedFrames( 0 ),
	m_elapsedTicks( 0 ),
	m_elapsedTacts( 0 )
{
	m_trackList.reserve( 1000 );
	connect( &m_tempoModel, SIGNAL( dataChanged() ),
						this, SLOT( setTempo() ) );
	connect( &m_tempoModel, SIGNAL( dataUnchanged() ),
						this, SLOT( setTempo() ) );
	connect( &m_timeSigModel, SIGNAL( dataChanged() ),
					this, SLOT( setTimeSignature() ) );

	m_tempoModel.setAutomationEnabled( false );
	m_timeSigModel.numeratorModel().setAutomationEnabled( false );
	m_timeSigModel.denominatorModel().setAutomationEnabled( false );
	m_timeSigModel.denominatorModel().setStepType( AutomatableModel::PowerOfTwoStep );

	connect( Engine::mixer(), SIGNAL( sampleRateChanged() ), this,
						SLOT( updateFramesPerTick() ) );

	connect( &m_masterVolumeModel, SIGNAL( dataChanged() ),
			this, SLOT( masterVolumeChanged() ) );
/*	connect( &m_masterPitchModel, SIGNAL( dataChanged() ),
			this, SLOT( masterPitchChanged() ) );*/

	qRegisterMetaType<Note>( "note" );
	
	m_lastTempo = m_tempoModel.value();
	m_playbackStartFpt = Engine::tempoToFramesPerTick( m_lastTempo );
	m_playbackStartTempo = m_lastTempo;
	m_vstSyncController.setTempo( m_lastTempo );
}




Song::~Song()
{
	m_playing = false;
}




void Song::masterVolumeChanged()
{
	Engine::mixer()->setMasterGain( m_masterVolumeModel.value() /
								100.0f );
}




void Song::setTempo() // called when the tempo widget is changed manually
{
	// if we're already playing the song, don't change the playback-start values
	if( ! ( m_playing && ( m_playMode == Mode_PlaySong || m_playMode == Mode_PlayTrack ) ) )
	{
		m_playbackStartTempo = m_tempoModel.value();
		Engine::updateFramesPerTick();
		m_playbackStartFpt = Engine::framesPerTick();
		m_vstSyncController.setTempo( m_tempoModel.value() ); // if we're not playing, we also need to update vst-sync here
	}
	/*
	Engine::mixer()->lockPlayHandleRemoval();
	const bpm_t tempo = (bpm_t) m_tempoModel.value();
	PlayHandleList & playHandles = Engine::mixer()->playHandles();
	for( PlayHandleList::Iterator it = playHandles.begin();
						it != playHandles.end(); ++it )
	{
		NotePlayHandle * nph = dynamic_cast<NotePlayHandle *>( *it );
		if( nph && !nph->isReleased() )
		{
			nph->lock();
			nph->resize( tempo );
			nph->unlock();
		}
	}
	Engine::mixer()->unlockPlayHandleRemoval();

	Engine::updateFramesPerTick();

	m_vstSyncController.setTempo( tempo );

	emit tempoChanged( tempo );
	*/
}


float Song::actualTempo() const
{
	// if we're not playing the song or a track, return the tempo widget tempo
	if( ! ( m_playing && ( m_playMode == Mode_PlaySong || m_playMode == Mode_PlayTrack ) ) )
	{
		return m_tempoModel.value();
	}
	// we are playing, so return tempo from tempomap
	return Engine::tempoAt( m_playPos[m_playMode] );
}


float Song::actualFpt() const
{
	// if we're not playing the song or a track, return value based on the tempo widget tempo
	if( ! ( m_playing && ( m_playMode == Mode_PlaySong || m_playMode == Mode_PlayTrack ) ) )
	{
		return Engine::framesPerTick();
	}
	// we are playing, so return tempo from tempomap
	return Engine::framesPerTick( m_playPos[m_playMode] );
}




void Song::setTimeSignature()
{
	MidiTime::setTicksPerTact( ticksPerTact() );
	emit timeSignatureChanged( m_oldTicksPerTact, ticksPerTact() );
	emit dataChanged();
	m_oldTicksPerTact = ticksPerTact();

	m_vstSyncController.setTimeSignature( getTimeSigModel().getNumerator(), getTimeSigModel().getDenominator() );
}




void Song::savePos()
{
	Timeline * tl = m_playPos[m_playMode].m_timeLine;

	if( tl != NULL )
	{
		tl->savePos( m_playPos[m_playMode] );
	}
}


float Song::elapsedMilliSeconds() const
{
	return m_elapsedFrames * 1000.0f / Engine::mixer()->processingSampleRate();
}



void Song::processNextBuffer()
{
	if( m_playing == false )
	{
		return;
	}

	m_trackList.clear();
	m_playingTcoNum = -1;
	
	m_ticksThisPeriod.clear();

	float tempo = m_lastTempo;
	bool useTempoMap = false;

	switch( m_playMode )
	{
		case Mode_PlaySong:
			qDebug( "tick %d, tempo %f, fpt %f", m_playPos[m_playMode].getTicks(), 
				Engine::tempoAt( m_playPos[m_playMode] ), Engine::framesPerTick( m_playPos[m_playMode] ) );
			m_trackList = tracks();
			// at song-start we have to reset the LFOs
			if( m_playPos[Mode_PlaySong] == 0 )
			{
				EnvelopeAndLfoParameters::instances()->reset();
			}
			useTempoMap = true;
			break;

		case Mode_PlayTrack:
			m_trackList.push_back( m_trackToPlay );
			// at song-start we have to reset the LFOs
			if( m_playPos[Mode_PlayTrack] == 0 )
			{
				EnvelopeAndLfoParameters::instances()->reset();
			}
			useTempoMap = true;
			break;

		case Mode_PlayBB:
			if( Engine::getBBTrackContainer()->numOfBBs() > 0 )
			{
				m_playingTcoNum = Engine::getBBTrackContainer()->
								currentBB();
				m_trackList.push_back( BBTrack::findBBTrack(
								m_playingTcoNum ) );
			}
			break;

		case Mode_PlayPattern:
			if( m_patternToPlay != NULL )
			{
				m_playingTcoNum = m_patternToPlay->getTrack()->
						getTCONum( m_patternToPlay );
				m_trackList.push_back(
						m_patternToPlay->getTrack() );
			}
			break;

		default:
			return;

	}
	
	// save the starting position of the period for further use
	m_periodStartPos[m_playMode] = m_playPos[m_playMode];

	if( m_trackList.empty() == true )
	{
		return;
	}

	float framesPerTick;

	// update tempo and fpt, either from tempomap or from tempo widget
	if( useTempoMap )
	{
		tempo = Engine::tempoAt( m_playPos[m_playMode] );
		framesPerTick = Engine::framesPerTick( m_playPos[m_playMode] );
	}
	else
	{
		tempo = m_tempoModel.value();
		framesPerTick = Engine::framesPerTick();
	}

	// check for looping-mode and act if necessary
	Timeline * tl = m_playPos[m_playMode].m_timeLine;
	bool check_loop = tl != NULL && m_exporting == false &&
				tl->loopPointsEnabled();
	if( check_loop )
	{
		if( m_playPos[m_playMode] < tl->loopBegin() ||
					m_playPos[m_playMode] >= tl->loopEnd() )
		{
			m_elapsedFrames = tl->loopBegin().getTicks() * framesPerTick;
			m_playPos[m_playMode].setTicks(	tl->loopBegin().getTicks() );
		}
	}

	f_cnt_t total_frames_played = 0;

	while( total_frames_played < Engine::mixer()->framesPerPeriod() )
	{
		// update vst sync if tempo has changed
		if( tempo != m_lastTempo )
		{
			m_vstSyncController.setTempo( tempo );
			m_lastTempo = tempo;
		}
		m_vstSyncController.update();

		f_cnt_t played_frames = Engine::mixer()->framesPerPeriod() - total_frames_played;

		float current_frame = m_playPos[m_playMode].currentFrame();
		// did we play a tick?
		if( current_frame >= framesPerTick )
		{
			int ticks = m_playPos[m_playMode].getTicks() + (int)( current_frame / framesPerTick );

			m_vstSyncController.setAbsolutePosition( ticks );

			// did we play a whole tact?
			if( ticks >= MidiTime::ticksPerTact() )
			{
				// per default we just continue playing even if
				// there's no more stuff to play
				// (song-play-mode)
				int max_tact = m_playPos[m_playMode].getTact() + 2;

				// then decide whether to go over to next tact
				// or to loop back to first tact
				if( m_playMode == Mode_PlayBB )
				{
					max_tact = Engine::getBBTrackContainer()->lengthOfCurrentBB();
				}
				else if( m_playMode == Mode_PlayPattern &&
					m_loopPattern == true &&
					tl != NULL &&
					tl->loopPointsEnabled() == false )
				{
					max_tact = m_patternToPlay->length().getTact();
				}

				// end of played object reached?
				if( m_playPos[m_playMode].getTact() + 1 >= max_tact )
				{
					// then start from beginning and keep
					// offset
					ticks = ticks % ( max_tact * MidiTime::ticksPerTact() );

					// wrap milli second counter
					m_elapsedFrames = ( ticks * framesPerTick );

					m_vstSyncController.setAbsolutePosition( ticks );
				}
			}
			m_playPos[m_playMode].setTicks( ticks );
			// if tempomap is used, update tempo and fpt
			if( useTempoMap )
			{
				tempo = Engine::tempoAt( m_playPos[m_playMode] );
				framesPerTick = Engine::framesPerTick( m_playPos[m_playMode] );
			}

			if( check_loop )
			{
				m_vstSyncController.startCycle( tl->loopBegin().getTicks(), tl->loopEnd().getTicks() );

				if( m_playPos[m_playMode] >= tl->loopEnd() )
				{
					m_playPos[m_playMode].setTicks( tl->loopBegin().getTicks() );
					m_elapsedFrames = tl->loopBegin().getTicks() * framesPerTick;
				}
			}
			else
			{
				m_vstSyncController.stopCycle();
			}

			current_frame = fmodf( current_frame, framesPerTick );
			m_playPos[m_playMode].setCurrentFrame( current_frame );
		}

		f_cnt_t last_frames = (f_cnt_t)framesPerTick - (f_cnt_t) current_frame;
		// skip last frame fraction
		if( last_frames == 0 )
		{
			++total_frames_played;
			m_playPos[m_playMode].setCurrentFrame( current_frame + 1.0f );
			continue;
		}
		// do we have some samples left in this tick but these are
		// less then samples we have to play?
		if( last_frames < played_frames )
		{
			// then set played_samples to remaining samples, the
			// rest will be played in next loop
			played_frames = last_frames;
		}

		if( (f_cnt_t) current_frame == 0 )
		{
			// save the tick's frame offset into a hash so tracks can read it and know which ticks to play
			m_ticksThisPeriod[ m_playPos[m_playMode].getTicks() ] = total_frames_played;
		}

		// update frame-counters
		total_frames_played += played_frames;
		m_playPos[m_playMode].setCurrentFrame( played_frames + current_frame );
		m_elapsedFrames += played_frames;
		m_elapsedTacts = m_playPos[Mode_PlaySong].getTact();
		m_elapsedTicks = (m_playPos[Mode_PlaySong].getTicks()%ticksPerTact())/48;
	}
}




void Song::playSong()
{
	m_recording = false;

	if( isStopped() == false )
	{
		stop();
	}

	m_playMode = Mode_PlaySong;
	m_playing = true;
	m_paused = false;

	m_vstSyncController.setPlaybackState( true );

	savePos();

	emit playbackStateChanged();
}




void Song::record()
{
	m_recording = true;
	// TODO: Implement
}




void Song::playAndRecord()
{
	playSong();
	m_recording = true;
}




void Song::playTrack( Track * _trackToPlay )
{
	if( isStopped() == false )
	{
		stop();
	}
	m_trackToPlay = _trackToPlay;

	m_playMode = Mode_PlayTrack;
	m_playing = true;
	m_paused = false;

	m_vstSyncController.setPlaybackState( true );

	savePos();

	emit playbackStateChanged();
}




void Song::playBB()
{
	if( isStopped() == false )
	{
		stop();
	}

	m_playMode = Mode_PlayBB;
	m_playing = true;
	m_paused = false;

	m_vstSyncController.setPlaybackState( true );

	savePos();

	emit playbackStateChanged();
}




void Song::playPattern( const Pattern* patternToPlay, bool _loop )
{
	if( isStopped() == false )
	{
		stop();
	}

	m_patternToPlay = patternToPlay;
	m_loopPattern = _loop;

	if( m_patternToPlay != NULL )
	{
		m_playMode = Mode_PlayPattern;
		m_playing = true;
		m_paused = false;
	}

	savePos();

	emit playbackStateChanged();
}




void Song::updateLength()
{
	m_length = 0;
	m_tracksMutex.lockForRead();
	for( TrackList::const_iterator it = tracks().begin();
						it != tracks().end(); ++it )
	{
		const tact_t cur = ( *it )->length();
		if( cur > m_length )
		{
			m_length = cur;
		}
	}
	m_tracksMutex.unlock();

	emit lengthChanged( m_length );
}




void Song::setPlayPos( tick_t _ticks, PlayModes _play_mode )
{
	m_elapsedTicks += m_playPos[_play_mode].getTicks() - _ticks;
	m_elapsedFrames = _ticks * Engine::framesPerTick(); // TODO 2.0 : fix this counter to use fpt map when appropriate
	m_playPos[_play_mode].setTicks( _ticks );
	m_playPos[_play_mode].setCurrentFrame( 0.0f );

// send a signal if playposition changes during playback
	if( isPlaying() ) 
	{
		emit playbackPositionChanged();
	}
}




void Song::togglePause()
{
	if( m_paused == true )
	{
		m_playing = true;
		m_paused = false;
	}
	else
	{
		m_playing = false;
		m_paused = true;
	}

	m_vstSyncController.setPlaybackState( m_playing );

	emit playbackStateChanged();
}




void Song::stop()
{
	// do not stop/reset things again if we're stopped already
	if( m_playMode == Mode_None )
	{
		return;
	}

	Timeline * tl = m_playPos[m_playMode].m_timeLine;
	m_playing = false;
	m_paused = false;
	m_recording = true;

	if( tl != NULL )
	{

		switch( tl->behaviourAtStop() )
		{
			case Timeline::BackToZero:
				m_playPos[m_playMode].setTicks( 0 );
				m_elapsedFrames = 0;
				break;

			case Timeline::BackToStart:
				if( tl->savedPos() >= 0 )
				{
					m_playPos[m_playMode].setTicks( tl->savedPos().getTicks() );
					m_elapsedFrames = tl->savedPos().getTicks() * Engine::framesPerTick();
					tl->savePos( -1 );
				}
				break;

			case Timeline::KeepStopPosition:
			default:
				break;
		}
	}
	else
	{
		m_playPos[m_playMode].setTicks( 0 );
		m_elapsedFrames = 0;
	}

	m_playPos[m_playMode].setCurrentFrame( 0 );

	m_vstSyncController.setPlaybackState( m_exporting );
	m_vstSyncController.setAbsolutePosition( m_playPos[m_playMode].getTicks() );

	// remove all note-play-handles that are active
	Engine::mixer()->clear();

	m_playMode = Mode_None;

	emit playbackStateChanged();
}




void Song::startExport()
{
	stop();
	m_playPos[Mode_PlaySong].setTicks( 0 );

	playSong();

	m_exporting = true;

	m_vstSyncController.setPlaybackState( true );
}




void Song::stopExport()
{
	stop();
	m_exporting = false;
	m_exportLoop = false;

	m_vstSyncController.setPlaybackState( m_playing );
}




void Song::insertBar()
{
	m_tracksMutex.lockForRead();
	for( TrackList::const_iterator it = tracks().begin();
					it != tracks().end(); ++it )
	{
		( *it )->insertTact( m_playPos[Mode_PlaySong] );
	}
	m_tracksMutex.unlock();
}




void Song::removeBar()
{
	m_tracksMutex.lockForRead();
	for( TrackList::const_iterator it = tracks().begin();
					it != tracks().end(); ++it )
	{
		( *it )->removeTact( m_playPos[Mode_PlaySong] );
	}
	m_tracksMutex.unlock();
}




void Song::addBBTrack()
{
	Track * t = Track::create( Track::BBTrack, this );
	Engine::getBBTrackContainer()->setCurrentBB( dynamic_cast<BBTrack *>( t )->index() );
}




void Song::addSampleTrack()
{
	(void) Track::create( Track::SampleTrack, this );
}




void Song::addAutomationTrack()
{
	(void) Track::create( Track::AutomationTrack, this );
}




float Song::getTempo()
{
	return m_tempoModel.value();
}




AutomationPattern * Song::tempoAutomationPattern()
{
	return NULL;
}




void Song::clearProject()
{
	Engine::projectJournal()->setJournalling( false );

	if( m_playing )
	{
		stop();
	}

	for( int i = 0; i < Mode_Count; i++ )
	{
		setPlayPos( 0, ( PlayModes )i );
	}


	Engine::mixer()->lock();
	if( Engine::getBBEditor() )
	{
		Engine::getBBEditor()->clearAllTracks();
	}
	if( Engine::songEditor() )
	{
		Engine::songEditor()->clearAllTracks();
	}
	if( Engine::fxMixerView() )
	{
		Engine::fxMixerView()->clear();
	}
	QCoreApplication::sendPostedEvents();
	Engine::getBBTrackContainer()->clearAllTracks();
	clearAllTracks();

	Engine::fxMixer()->clear();

	if( Engine::automationEditor() )
	{
		Engine::automationEditor()->setCurrentPattern( NULL );
	}

	if( Engine::pianoRoll() )
	{
		Engine::pianoRoll()->reset();
	}

	m_tempoModel.reset();
	m_masterVolumeModel.reset();
	m_masterPitchModel.reset();
	m_timeSigModel.reset();

	Engine::mixer()->unlock();

	if( Engine::getProjectNotes() )
	{
		Engine::getProjectNotes()->clear();
	}

	// Move to function
	while( !m_controllers.empty() )
	{
		delete m_controllers.last();
	}

	// new tempo track
	Track::create( Track::TempoTrack, this );

	emit dataChanged();

	Engine::projectJournal()->clearJournal();

	Engine::projectJournal()->setJournalling( true );

	InstrumentTrackView::cleanupWindowCache();
}





// create new file
void Song::createNewProject()
{
	QString default_template = ConfigManager::inst()->userProjectsDir()
						+ "templates/default.mpt";

	if( QFile::exists( default_template ) )
	{
		createNewProjectFromTemplate( default_template );
		return;
	}

	default_template = ConfigManager::inst()->factoryProjectsDir()
						+ "templates/default.mpt";
	if( QFile::exists( default_template ) )
	{
		createNewProjectFromTemplate( default_template );
		return;
	}

	m_loadingProject = true;

	clearProject();

	Engine::projectJournal()->setJournalling( false );

	m_fileName = m_oldFileName = "";

	Track * t;
	t = Track::create( Track::InstrumentTrack, this );
	dynamic_cast<InstrumentTrack * >( t )->loadInstrument(
					"tripleoscillator" );
	t = Track::create( Track::InstrumentTrack,
						Engine::getBBTrackContainer() );
	dynamic_cast<InstrumentTrack * >( t )->loadInstrument(
						"kicker" );
	Track::create( Track::SampleTrack, this );
	Track::create( Track::BBTrack, this );
	Track::create( Track::AutomationTrack, this );

	m_tempoModel.setInitValue( DefaultTempo );
	m_timeSigModel.reset();
	m_masterVolumeModel.setInitValue( 100 );
	m_masterPitchModel.setInitValue( 0 );

	QCoreApplication::instance()->processEvents();

	m_loadingProject = false;

	Engine::getBBTrackContainer()->updateAfterTrackAdd();

	Engine::projectJournal()->setJournalling( true );

	QCoreApplication::sendPostedEvents();

	m_modified = false;

	if( Engine::mainWindow() )
	{
		Engine::mainWindow()->resetWindowTitle();
	}
}




void Song::createNewProjectFromTemplate( const QString & _template )
{
	loadProject( _template );
	// clear file-name so that user doesn't overwrite template when
	// saving...
	m_fileName = m_oldFileName = "";
	// update window title
	if( Engine::mainWindow() )
	{
		Engine::mainWindow()->resetWindowTitle();
	}

}




// load given song
void Song::loadProject( const QString & _file_name )
{
	QDomNode node;

	m_loadingProject = true;

	Engine::projectJournal()->setJournalling( false );
	Engine::mainWindow()->clearErrors();

	m_fileName = _file_name;
	m_oldFileName = _file_name;

	DataFile dataFile( m_fileName );
	// if file could not be opened, head-node is null and we create
	// new project
	if( dataFile.head().isNull() )
	{
		return;
	}

	clearProject();

	DataFile::LocaleHelper localeHelper( DataFile::LocaleHelper::ModeLoad );

	Engine::mixer()->lock();

	// get the header information from the DOM
	m_tempoModel.loadSettings( dataFile.head(), "bpm" );
	m_timeSigModel.loadSettings( dataFile.head(), "timesig" );
	m_masterVolumeModel.loadSettings( dataFile.head(), "mastervol" );
	m_masterPitchModel.loadSettings( dataFile.head(), "masterpitch" );

	if( m_playPos[Mode_PlaySong].m_timeLine )
	{
		// reset loop-point-state
		m_playPos[Mode_PlaySong].m_timeLine->toggleLoopPoints( 0 );
	}

	// Load mixer first to be able to set the correct range for FX channels
	node = dataFile.content().firstChildElement( Engine::fxMixer()->nodeName() );
	if( !node.isNull() )
	{
		Engine::fxMixer()->restoreState( node.toElement() );
		if( Engine::hasGUI() )
		{
			// refresh FxMixerView
			Engine::fxMixerView()->refreshDisplay();
		}
	}

	node = dataFile.content().firstChild();
	while( !node.isNull() )
	{
		if( node.isElement() )
		{
			if( node.nodeName() == "trackcontainer" )
			{
				( (JournallingObject *)( this ) )->restoreState( node.toElement() );
			}
			else if( node.nodeName() == "controllers" )
			{
				restoreControllerStates( node.toElement() );
			}
			else if( Engine::hasGUI() )
			{
				if( node.nodeName() == Engine::getControllerRackView()->nodeName() )
				{
					Engine::getControllerRackView()->restoreState( node.toElement() );
				}
				else if( node.nodeName() == Engine::pianoRoll()->nodeName() )
				{
					Engine::pianoRoll()->restoreState( node.toElement() );
				}
				else if( node.nodeName() == Engine::automationEditor()->nodeName() )
				{
					Engine::automationEditor()->restoreState( node.toElement() );
				}
				else if( node.nodeName() == Engine::getProjectNotes()->nodeName() )
				{
					 Engine::getProjectNotes()->SerializingObject::restoreState( node.toElement() );
				}
				else if( node.nodeName() == m_playPos[Mode_PlaySong].m_timeLine->nodeName() )
				{
					m_playPos[Mode_PlaySong].m_timeLine->restoreState( node.toElement() );
				}
			}
		}
		node = node.nextSibling();
	}

	// quirk for fixing projects with broken positions of TCOs inside
	// BB-tracks
	Engine::getBBTrackContainer()->fixIncorrectPositions();

	// Connect controller links to their controllers 
	// now that everything is loaded
	ControllerConnection::finalizeConnections();

	// resolve all IDs so that autoModels are automated
	AutomationTrack::resolveAllIDs();


	Engine::mixer()->unlock();

	ConfigManager::inst()->addRecentlyOpenedProject( _file_name );

	Engine::projectJournal()->setJournalling( true );

	emit projectLoaded();

	Engine::mainWindow()->showErrors( tr( "The following errors occured while loading: " ) );

	m_loadingProject = false;
	m_modified = false;

	if( Engine::mainWindow() )
	{
		Engine::mainWindow()->resetWindowTitle();
	}
}


// only save current song as _filename and do nothing else
bool Song::saveProjectFile( const QString & _filename )
{
	DataFile::LocaleHelper localeHelper( DataFile::LocaleHelper::ModeSave );

	DataFile dataFile( DataFile::SongProject );

	m_tempoModel.saveSettings( dataFile, dataFile.head(), "bpm" );
	m_timeSigModel.saveSettings( dataFile, dataFile.head(), "timesig" );
	m_masterVolumeModel.saveSettings( dataFile, dataFile.head(), "mastervol" );
	m_masterPitchModel.saveSettings( dataFile, dataFile.head(), "masterpitch" );

	saveState( dataFile, dataFile.content() );

	Engine::fxMixer()->saveState( dataFile, dataFile.content() );
	if( Engine::hasGUI() )
	{
		Engine::getControllerRackView()->saveState( dataFile, dataFile.content() );
		Engine::pianoRoll()->saveState( dataFile, dataFile.content() );
		Engine::automationEditor()->saveState( dataFile, dataFile.content() );
		Engine::getProjectNotes()->SerializingObject::saveState( dataFile, dataFile.content() );
		m_playPos[Mode_PlaySong].m_timeLine->saveState( dataFile, dataFile.content() );
	}

	saveControllerStates( dataFile, dataFile.content() );

	return dataFile.writeFile( _filename );
}



// save current song and update the gui
bool Song::guiSaveProject()
{
	DataFile dataFile( DataFile::SongProject );
	m_fileName = dataFile.nameWithExtension( m_fileName );
	if( saveProjectFile( m_fileName ) && Engine::hasGUI() )
	{
		TextFloat::displayMessage( tr( "Project saved" ),
					tr( "The project %1 is now saved."
							).arg( m_fileName ),
				embed::getIconPixmap( "project_save", 24, 24 ),
									2000 );
		ConfigManager::inst()->addRecentlyOpenedProject( m_fileName );
		m_modified = false;
		Engine::mainWindow()->resetWindowTitle();
	}
	else if( Engine::hasGUI() )
	{
		TextFloat::displayMessage( tr( "Project NOT saved." ),
				tr( "The project %1 was not saved!" ).arg(
							m_fileName ),
				embed::getIconPixmap( "error" ), 4000 );
		return false;
	}

	return true;
}




// save current song in given filename
bool Song::guiSaveProjectAs( const QString & _file_name )
{
	QString o = m_oldFileName;
	m_oldFileName = m_fileName;
	m_fileName = _file_name;
	if( guiSaveProject() == false )
	{
		m_fileName = m_oldFileName;
		m_oldFileName = o;
		return false;
	}
	m_oldFileName = m_fileName;
	return true;
}




void Song::importProject()
{
	FileDialog ofd( NULL, tr( "Import file" ),
			ConfigManager::inst()->userProjectsDir(),
			tr("MIDI sequences") +
			" (*.mid *.midi *.rmi);;" +
			tr("FL Studio projects") +
			" (*.flp);;" +
			tr("Hydrogen projects") +
			" (*.h2song);;" +
			tr("All file types") +
			" (*.*)");

	ofd.setFileMode( FileDialog::ExistingFiles );
	if( ofd.exec () == QDialog::Accepted && !ofd.selectedFiles().isEmpty() )
	{
		ImportFilter::import( ofd.selectedFiles()[0], this );
	}
}




void Song::saveControllerStates( QDomDocument & _doc, QDomElement & _this )
{
	// save settings of controllers
	QDomElement controllersNode =_doc.createElement( "controllers" );
	_this.appendChild( controllersNode );
	for( int i = 0; i < m_controllers.size(); ++i )
	{
		m_controllers[i]->saveState( _doc, controllersNode );
	}
}




void Song::restoreControllerStates( const QDomElement & _this )
{
	QDomNode node = _this.firstChild();
	while( !node.isNull() )
	{
		Controller * c = Controller::create( node.toElement(), this );
		Q_ASSERT( c != NULL );

		/* For PeakController, addController() was called in
		 * PeakControllerEffect::PeakControllerEffect().
		 * This line removes the previously added controller for PeakController
		 * without affecting the order of controllers in Controller Rack
		 */
		Engine::getSong()->removeController( c );
		addController( c );

		node = node.nextSibling();
	}
}


void Song::exportProjectTracks()
{
	exportProject(true);
}

void Song::exportProject(bool multiExport)
{
	if( isEmpty() )
	{
		QMessageBox::information( Engine::mainWindow(),
				tr( "Empty project" ),
				tr( "This project is empty so exporting makes "
					"no sense. Please put some items into "
					"Song Editor first!" ) );
		return;
	}

	FileDialog efd( Engine::mainWindow() );
	if (multiExport)
	{
		efd.setFileMode( FileDialog::Directory);
		efd.setWindowTitle( tr( "Select directory for writing exported tracks..." ) );
		if( !m_fileName.isEmpty() )
		{
			efd.setDirectory( QFileInfo( m_fileName ).absolutePath() );
		}
	}
	else
	{
		efd.setFileMode( FileDialog::AnyFile );
		int idx = 0;
		QStringList types;
		while( __fileEncodeDevices[idx].m_fileFormat != ProjectRenderer::NumFileFormats )
		{
			types << tr( __fileEncodeDevices[idx].m_description );
			++idx;
		}
		efd.setNameFilters( types );
		QString base_filename;
		if( !m_fileName.isEmpty() )
		{
			efd.setDirectory( QFileInfo( m_fileName ).absolutePath() );
			base_filename = QFileInfo( m_fileName ).completeBaseName();
		}
		else
		{
			efd.setDirectory( ConfigManager::inst()->userProjectsDir() );
			base_filename = tr( "untitled" );
		}
		efd.selectFile( base_filename + __fileEncodeDevices[0].m_extension );
		efd.setWindowTitle( tr( "Select file for project-export..." ) );
	}

	efd.setAcceptMode( FileDialog::AcceptSave );


	if( efd.exec() == QDialog::Accepted && !efd.selectedFiles().isEmpty() && !efd.selectedFiles()[0].isEmpty() )
	{
		QString suffix = "";
		if ( !multiExport )
		{
			int stx = efd.selectedNameFilter().indexOf( "(*." );
			int etx = efd.selectedNameFilter().indexOf( ")" );
	
			if ( stx > 0 && etx > stx ) 
			{
				// Get first extension from selected dropdown.
				// i.e. ".wav" from "WAV-File (*.wav), Dummy-File (*.dum)"
				suffix = efd.selectedNameFilter().mid( stx + 2, etx - stx - 2 ).split( " " )[0].trimmed();
				if ( efd.selectedFiles()[0].endsWith( suffix ) )
				{
					suffix = "";
				}
			}
		}

		const QString export_file_name = efd.selectedFiles()[0] + suffix;
		ExportProjectDialog epd( export_file_name, Engine::mainWindow(), multiExport );
		epd.exec();
	}
}




void Song::updateFramesPerTick()
{
	Engine::updateFramesPerTick();
}




void Song::setModified()
{
	if( !m_loadingProject )
	{
		m_modified = true;
		if( Engine::mainWindow() &&
			QThread::currentThread() == Engine::mainWindow()->thread() )
		{
			Engine::mainWindow()->resetWindowTitle();
		}
	}
}




void Song::addController( Controller * _c )
{
	if( _c != NULL && !m_controllers.contains( _c ) ) 
	{
		m_controllers.append( _c );
		emit dataChanged();
	}
}




void Song::removeController( Controller * _controller )
{
	int index = m_controllers.indexOf( _controller );
	if( index != -1 )
	{
		m_controllers.remove( index );

		if( Engine::getSong() )
		{
			Engine::getSong()->setModified();
		}
		emit dataChanged();
	}
}




