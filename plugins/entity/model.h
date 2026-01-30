/*
   Copyright (C) 2001-2006, William Joseph.
   All Rights Reserved.

   This file is part of GtkRadiant.

   GtkRadiant is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   GtkRadiant is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with GtkRadiant; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#pragma once

#include "entitylib.h"
#include "traverselib.h"
#include "generic/callback.h"
#include "stream/stringstream.h"
#include "os/path.h"
#include "moduleobserver.h"

class EModel : public ModuleObserver
{
	ResourceReference m_resource;
	scene::Traversable& m_traverse;
	scene::Node* m_node;
	bool m_realised;
	Callback<void()> m_modelChanged;

public:
	EModel( scene::Traversable& traversable, const Callback<void()>& modelChanged )
		: m_resource( "" ), m_traverse( traversable ), m_node( 0 ), m_realised( false ), m_modelChanged( modelChanged ){
		m_resource.attach( *this );
	}
	~EModel(){
		m_resource.detach( *this );
	}

	void realise() override {
		if ( m_realised ) {
			return;
		}
		const bool loaded = m_resource.get()->load();
		if ( !loaded ) {
			m_node = 0;
			return;
		}
		m_node = m_resource.get()->getNode();
		if ( m_node != 0 ) {
			m_traverse.insert( *m_node );
			m_realised = true;
		}
	}
	void unrealise() override {
		if ( !m_realised ) {
			m_node = 0;
			return;
		}
		if ( m_node != 0 ) {
			m_traverse.erase( *m_node );
		}
		m_node = 0;
		m_realised = false;
	}

	void modelChanged( const char* value ){
		m_resource.detach( *this );
		m_resource.setName( StringOutputStream( string_length( value ) + 1 )( PathCleaned( value ) ) );
		m_resource.attach( *this );
		m_modelChanged();
	}
	typedef MemberCaller<EModel, void(const char*), &EModel::modelChanged> ModelChangedCaller;

	const char* getName() const {
		return m_resource.getName();
	}
	scene::Node* getNode() const {
		return m_node;
	}
};

class SingletonModel
{
	TraversableNode m_traverse;
	EModel m_model;
public:
	SingletonModel()
		: m_model( m_traverse, Callback<void()>() ){
	}

	void attach( scene::Traversable::Observer* observer ){
		m_traverse.attach( observer );
	}
	void detach( scene::Traversable::Observer* observer ){
		m_traverse.detach( observer );
	}

	scene::Traversable& getTraversable(){
		return m_traverse;
	}

	void modelChanged( const char* value ){
		m_model.modelChanged( value );
	}
	typedef MemberCaller<SingletonModel, void(const char*), &SingletonModel::modelChanged> ModelChangedCaller;

	scene::Node* getNode() const {
		return m_model.getNode();
	}
};

class MultiModel
{
	TraversableNodeSet m_traverse;
	EModel m_primary;
	EModel m_secondary;
public:
	MultiModel()
		: m_primary( m_traverse, Callback<void()>() ),
		  m_secondary( m_traverse, Callback<void()>() ){
	}

	void attach( scene::Traversable::Observer* observer ){
		m_traverse.attach( observer );
	}
	void detach( scene::Traversable::Observer* observer ){
		m_traverse.detach( observer );
	}

	scene::Traversable& getTraversable(){
		return m_traverse;
	}

	void modelChangedPrimary( const char* value ){
		m_primary.modelChanged( value );
		if ( !string_empty( value ) && string_equal_nocase( value, m_secondary.getName() ) ) {
			m_secondary.modelChanged( "" );
		}
	}
	void modelChangedSecondary( const char* value ){
		m_secondary.modelChanged( value );
		if ( !string_empty( value ) && string_equal_nocase( value, m_primary.getName() ) ) {
			m_primary.modelChanged( "" );
		}
	}
	void setModels( const char* primary, const char* secondary ){
		const char* primaryName = primary != nullptr ? primary : "";
		const char* secondaryName = secondary != nullptr ? secondary : "";
		if ( !string_empty( primaryName ) && !string_empty( secondaryName )
		  && string_equal_nocase( primaryName, secondaryName ) ) {
			secondaryName = "";
		}
		m_primary.modelChanged( primaryName );
		m_secondary.modelChanged( secondaryName );
	}

	scene::Node* getPrimaryNode() const {
		return m_primary.getNode();
	}
	scene::Node* getSecondaryNode() const {
		return m_secondary.getNode();
	}
};
