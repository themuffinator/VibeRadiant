/*
   Copyright (C) 2026
*/

#pragma once

class QNetworkRequest;
class QByteArray;

void GenAI_Construct();
void GenAI_Destroy();

bool GenAI_IsEnabled();
bool GenAI_IsConfigured();
bool GenAI_PrepareOpenAIRequest( QNetworkRequest& request, QByteArray& error );
QByteArray GenAI_BuildSimpleResponsesPayload( const char* prompt );

