/*
   Copyright (C) 2026, VibeRadiant contributors.

   This file is part of VibeRadiant.
*/

#include "issuebrowser.h"

#include "ientity.h"
#include "iscenegraph.h"
#include "iselection.h"
#include "iundo.h"
#include "map.h"
#include "scenelib.h"
#include "select.h"
#include "string/string.h"

#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cctype>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
enum class IssueKind
{
	MissingClassname,
	DuplicateTargetname,
	MissingTargetReference,
};

struct IssueEntry
{
	IssueKind kind;
	std::string value;
	int count = 0;
};

struct EntityRecord
{
	Entity* entity = nullptr;
	std::string classname;
	std::string target;
	std::string targetname;
};

inline std::string trim_copy( const std::string& value ){
	std::size_t begin = 0;
	while( begin < value.size() && std::isspace( static_cast<unsigned char>( value[begin] ) ) ){
		++begin;
	}
	std::size_t end = value.size();
	while( end > begin && std::isspace( static_cast<unsigned char>( value[end - 1] ) ) ){
		--end;
	}
	return value.substr( begin, end - begin );
}

inline bool key_empty( const char* value ){
	return value == nullptr || trim_copy( value ).empty();
}

inline std::string make_unique_targetname( const std::string& base, std::unordered_set<std::string>& usedNames ){
	const std::string stem = base.empty() ? std::string( "target" ) : base;
	std::size_t suffix = 1;
	for( ;; ){
		std::string candidate = stem + "_" + std::to_string( suffix++ );
		if( usedNames.insert( candidate ).second ){
			return candidate;
		}
	}
}

std::vector<EntityRecord> collect_entities(){
	std::vector<EntityRecord> entities;

	class EntityCollectWalker final : public scene::Graph::Walker
	{
		std::vector<EntityRecord>& m_entities;
		mutable std::set<scene::Node*> m_seen;
	public:
		explicit EntityCollectWalker( std::vector<EntityRecord>& entities ) : m_entities( entities ){
		}
		bool pre( const scene::Path& path, scene::Instance& instance ) const override {
			scene::Node& node = path.top().get();
			if( !m_seen.insert( &node ).second ){
				return false;
			}
			if( Entity* entity = Node_getEntity( node ) ){
				EntityRecord record;
				record.entity = entity;
				record.classname = trim_copy( entity->getKeyValue( "classname" ) );
				record.target = trim_copy( entity->getKeyValue( "target" ) );
				record.targetname = trim_copy( entity->getKeyValue( "targetname" ) );
				m_entities.push_back( std::move( record ) );
			}
			return true;
		}
	};

	GlobalSceneGraph().traverse( EntityCollectWalker( entities ) );
	return entities;
}

const char* issue_kind_text( IssueKind kind ){
	switch ( kind )
	{
	case IssueKind::MissingClassname:
		return "Missing classname";
	case IssueKind::DuplicateTargetname:
		return "Duplicate targetname";
	case IssueKind::MissingTargetReference:
		return "Broken target reference";
	}
	return "Issue";
}

const char* issue_severity_text( IssueKind kind ){
	switch ( kind )
	{
	case IssueKind::MissingClassname:
		return "Error";
	case IssueKind::DuplicateTargetname:
	case IssueKind::MissingTargetReference:
		return "Warning";
	}
	return "Info";
}

std::vector<IssueEntry> build_issues(){
	const std::vector<EntityRecord> entities = collect_entities();
	std::vector<IssueEntry> issues;

	std::unordered_map<std::string, int> targetnameCounts;
	std::unordered_set<std::string> allTargetnames;
	int missingClassnameCount = 0;

	for( const EntityRecord& entity : entities ){
		if( entity.classname.empty() ){
			++missingClassnameCount;
		}
		if( !entity.targetname.empty() ){
			++targetnameCounts[entity.targetname];
			allTargetnames.insert( entity.targetname );
		}
	}

	if( missingClassnameCount > 0 ){
		issues.push_back( { IssueKind::MissingClassname, "", missingClassnameCount } );
	}

	for( const auto& [targetname, count] : targetnameCounts ){
		if( count > 1 ){
			issues.push_back( { IssueKind::DuplicateTargetname, targetname, count } );
		}
	}

	std::unordered_map<std::string, int> missingTargetCounts;
	for( const EntityRecord& entity : entities ){
		if( !entity.target.empty() && !allTargetnames.count( entity.target ) ){
			++missingTargetCounts[entity.target];
		}
	}
	for( const auto& [missingTarget, count] : missingTargetCounts ){
		issues.push_back( { IssueKind::MissingTargetReference, missingTarget, count } );
	}

	std::sort( issues.begin(), issues.end(), []( const IssueEntry& a, const IssueEntry& b ){
		if( a.kind != b.kind ){
			return static_cast<int>( a.kind ) < static_cast<int>( b.kind );
		}
		if( a.count != b.count ){
			return a.count > b.count;
		}
		return string_less_nocase( a.value.c_str(), b.value.c_str() );
	} );

	return issues;
}

class IssueBrowserPanel final : public QWidget
{
	QLabel* m_summary = nullptr;
	QTreeWidget* m_tree = nullptr;
	std::vector<IssueEntry> m_issues;

	int fix_missing_classname() const{
		const std::vector<EntityRecord> entities = collect_entities();
		std::vector<Entity*> toFix;
		toFix.reserve( entities.size() );
		for( const EntityRecord& entity : entities ){
			if( entity.classname.empty() ){
				toFix.push_back( entity.entity );
			}
		}
		if( toFix.empty() ){
			return 0;
		}

		UndoableCommand undo( "Issue Browser: assign classname" );
		for( Entity* entity : toFix ){
			entity->setKeyValue( "classname", "info_null" );
		}
		SceneChangeNotify();
		return static_cast<int>( toFix.size() );
	}

	int fix_duplicate_targetname( const std::string& duplicatedName ) const{
		if( duplicatedName.empty() ){
			return 0;
		}

		const std::vector<EntityRecord> entities = collect_entities();
		std::vector<Entity*> duplicates;
		std::unordered_set<std::string> usedNames;
		for( const EntityRecord& entity : entities ){
			if( !entity.targetname.empty() ){
				usedNames.insert( entity.targetname );
			}
			if( entity.targetname == duplicatedName ){
				duplicates.push_back( entity.entity );
			}
		}
		if( duplicates.size() <= 1 ){
			return 0;
		}

		UndoableCommand undo( "Issue Browser: uniquify targetname" );
		int changed = 0;
		for( std::size_t i = 1; i < duplicates.size(); ++i ){
			const std::string uniqueName = make_unique_targetname( duplicatedName, usedNames );
			duplicates[i]->setKeyValue( "targetname", uniqueName.c_str() );
			++changed;
		}
		SceneChangeNotify();
		return changed;
	}

	int fix_missing_target_reference( const std::string& targetValue ) const{
		if( targetValue.empty() ){
			return 0;
		}

		const std::vector<EntityRecord> entities = collect_entities();
		std::unordered_set<std::string> allTargetnames;
		for( const EntityRecord& entity : entities ){
			if( !entity.targetname.empty() ){
				allTargetnames.insert( entity.targetname );
			}
		}
		if( allTargetnames.count( targetValue ) ){
			return 0;
		}

		std::vector<Entity*> toFix;
		for( const EntityRecord& entity : entities ){
			if( entity.target == targetValue ){
				toFix.push_back( entity.entity );
			}
		}
		if( toFix.empty() ){
			return 0;
		}

		UndoableCommand undo( "Issue Browser: clear broken target" );
		for( Entity* entity : toFix ){
			entity->setKeyValue( "target", "" );
		}
		SceneChangeNotify();
		return static_cast<int>( toFix.size() );
	}

	void select_missing_classname_entities() const{
		GlobalSelectionSystem().setSelectedAll( false );

		class SelectMissingClassnameWalker final : public scene::Graph::Walker
		{
			mutable std::set<scene::Node*> m_seen;
		public:
			bool pre( const scene::Path& path, scene::Instance& instance ) const override {
				scene::Node& node = path.top().get();
				if( !m_seen.insert( &node ).second ){
					return false;
				}
				Entity* entity = Node_getEntity( node );
				if( entity != nullptr && key_empty( entity->getKeyValue( "classname" ) ) ){
					if( Selectable* selectable = Instance_getSelectable( instance ) ){
						selectable->setSelected( true );
					}
				}
				return true;
			}
		};

		GlobalSceneGraph().traverse( SelectMissingClassnameWalker() );
	}

	int apply_issue( const IssueEntry& issue ) const{
		switch ( issue.kind )
		{
		case IssueKind::MissingClassname:
			return fix_missing_classname();
		case IssueKind::DuplicateTargetname:
			return fix_duplicate_targetname( issue.value );
		case IssueKind::MissingTargetReference:
			return fix_missing_target_reference( issue.value );
		}
		return 0;
	}

	void select_issue( const IssueEntry& issue ) const{
		switch ( issue.kind )
		{
		case IssueKind::MissingClassname:
			select_missing_classname_entities();
			break;
		case IssueKind::DuplicateTargetname:
			GlobalSelectionSystem().setSelectedAll( false );
			Select_EntitiesByKeyValue( "targetname", issue.value.c_str() );
			break;
		case IssueKind::MissingTargetReference:
			GlobalSelectionSystem().setSelectedAll( false );
			Select_EntitiesByKeyValue( "target", issue.value.c_str() );
			break;
		}
	}

	const IssueEntry* current_issue() const{
		if( QTreeWidgetItem* item = m_tree->currentItem() ){
			const int index = item->data( 0, Qt::ItemDataRole::UserRole ).toInt();
			if( index >= 0 && static_cast<std::size_t>( index ) < m_issues.size() ){
				return &m_issues[static_cast<std::size_t>( index )];
			}
		}
		return nullptr;
	}

	void rebuild_issue_tree(){
		m_tree->clear();
		for( std::size_t i = 0; i < m_issues.size(); ++i ){
			const IssueEntry& issue = m_issues[i];
			auto* item = new QTreeWidgetItem( m_tree );
			item->setText( 0, issue_severity_text( issue.kind ) );
			item->setText( 1, issue_kind_text( issue.kind ) );
			item->setText( 2, issue.value.empty() ? "-" : issue.value.c_str() );
			item->setText( 3, QString::number( issue.count ) );
			item->setData( 0, Qt::ItemDataRole::UserRole, static_cast<int>( i ) );
		}
		if( m_tree->topLevelItemCount() > 0 ){
			m_tree->setCurrentItem( m_tree->topLevelItem( 0 ) );
		}
	}

	void update_summary(){
		int affectedCount = 0;
		for( const IssueEntry& issue : m_issues ){
			affectedCount += issue.count;
		}
		if( m_issues.empty() ){
			m_summary->setText( "No map issues found." );
		}
		else{
			m_summary->setText( QString( "%1 issue groups, %2 affected entities" )
			                    .arg( m_issues.size() )
			                    .arg( affectedCount ) );
		}
	}

public:
	explicit IssueBrowserPanel( QWidget* parent ) : QWidget( parent ){
		auto* mainLayout = new QVBoxLayout( this );
		mainLayout->setContentsMargins( 6, 6, 6, 6 );
		mainLayout->setSpacing( 6 );

		auto* topLayout = new QHBoxLayout;
		m_summary = new QLabel( this );
		m_summary->setText( "Scan to discover map issues." );
		topLayout->addWidget( m_summary, 1 );

		auto* scanButton = new QPushButton( "Scan", this );
		topLayout->addWidget( scanButton );
		mainLayout->addLayout( topLayout );

		m_tree = new QTreeWidget( this );
		m_tree->setColumnCount( 4 );
		m_tree->setUniformRowHeights( true );
		m_tree->setRootIsDecorated( false );
		m_tree->setAlternatingRowColors( true );
		m_tree->setSelectionMode( QAbstractItemView::SelectionMode::SingleSelection );
		m_tree->setHeaderLabels( { "Severity", "Issue", "Value", "Count" } );
		m_tree->header()->setSectionResizeMode( QHeaderView::ResizeMode::ResizeToContents );
		m_tree->header()->setStretchLastSection( true );
		mainLayout->addWidget( m_tree, 1 );

		auto* buttonsLayout = new QHBoxLayout;
		auto* selectButton = new QPushButton( "Select Affected", this );
		auto* fixSelectedButton = new QPushButton( "Fix Selected", this );
		auto* fixAllButton = new QPushButton( "Fix All", this );
		buttonsLayout->addWidget( selectButton );
		buttonsLayout->addWidget( fixSelectedButton );
		buttonsLayout->addWidget( fixAllButton );
		mainLayout->addLayout( buttonsLayout );

		QObject::connect( scanButton, &QPushButton::clicked, this, [this](){
			m_issues = build_issues();
			rebuild_issue_tree();
			update_summary();
		} );
		QObject::connect( selectButton, &QPushButton::clicked, this, [this](){
			if( const IssueEntry* issue = current_issue(); issue != nullptr ){
				select_issue( *issue );
			}
		} );
		QObject::connect( fixSelectedButton, &QPushButton::clicked, this, [this](){
			if( const IssueEntry* issue = current_issue(); issue != nullptr ){
				apply_issue( *issue );
				m_issues = build_issues();
				rebuild_issue_tree();
				update_summary();
			}
		} );
		QObject::connect( fixAllButton, &QPushButton::clicked, this, [this](){
			const std::vector<IssueEntry> issuesToFix = m_issues;
			for( const IssueEntry& issue : issuesToFix ){
				apply_issue( issue );
			}
			m_issues = build_issues();
			rebuild_issue_tree();
			update_summary();
		} );
		QObject::connect( m_tree, &QTreeWidget::itemDoubleClicked, this, [this]( QTreeWidgetItem*, int ){
			if( const IssueEntry* issue = current_issue(); issue != nullptr ){
				select_issue( *issue );
			}
		} );

		m_issues = build_issues();
		rebuild_issue_tree();
		update_summary();
	}
};

IssueBrowserPanel* g_issueBrowserPanel = nullptr;
}

QWidget* IssueBrowser_constructWindow( QWidget* toplevel ){
	if( g_issueBrowserPanel == nullptr ){
		g_issueBrowserPanel = new IssueBrowserPanel( toplevel );
		QObject::connect( g_issueBrowserPanel, &QObject::destroyed, []( QObject* ){
			g_issueBrowserPanel = nullptr;
		} );
	}
	return g_issueBrowserPanel;
}

void IssueBrowser_destroyWindow(){
	g_issueBrowserPanel = nullptr;
}
