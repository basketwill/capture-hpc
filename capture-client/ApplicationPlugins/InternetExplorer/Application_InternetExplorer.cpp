/*
 *	PROJECT: Capture
 *	FILE: Client_InternetExplorer.cpp
 *	AUTHORS: Ramon Steenson (rsteenson@gmail.com) & Christian Seifert (christian.seifert@gmail.com)
 *
 *	Developed by Victoria University of Wellington and the New Zealand Honeynet Alliance
 *
 *	This file is part of Capture.
 *
 *	Capture is free software; you can redistribute it and/or modify
 *	it under the terms of the GNU General Public License as published by
 *	the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  Capture is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with Capture; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */
#include "Application_InternetExplorer.h"
#include "InternetExplorerInstance.h"

static HANDLE worker_threads[MAX_WORKER_THREADS]; 
static bool worker_thread_busy[MAX_WORKER_THREADS];
static LPVOID* worker_thread_data[MAX_WORKER_THREADS];
static HANDLE worker_has_data[MAX_WORKER_THREADS];
static HANDLE worker_finished[MAX_WORKER_THREADS]; 

struct VISIT_INFO
{
	InternetExplorerInstance* internet_explorer_instance;
	Url* url;
};

DWORD WINAPI 
Application_InternetExplorer::InternetExplorerWorker(LPVOID data)
{
	int worker_id = (int)data;
	while(true)
	{
		
		WaitForSingleObject(worker_has_data[worker_id], INFINITE);
		

		VISIT_INFO* visit_information = (VISIT_INFO*)worker_thread_data[worker_id];

		_ASSERT(visit_information);

		if(visit_information)
		{
			// Get the visit information
			Url* url = visit_information->url;
			InternetExplorerInstance* internet_explorer_instance = visit_information->internet_explorer_instance;
			
			// Visit the actual url
			internet_explorer_instance->visitUrl(url);
			worker_thread_busy[worker_id] = false;
			SetEvent(worker_finished[worker_id]);
		}
	}
}

Application_InternetExplorer::Application_InternetExplorer()
{
	// Reset worker threads status
	for(unsigned int i = 0; i < MAX_WORKER_THREADS; i++)
	{
		worker_thread_busy[i] = false;
		worker_thread_data[i] = NULL;
		worker_has_data[i] = CreateEvent(NULL, false, NULL, NULL);
		worker_finished[i] = CreateEvent(NULL, false, NULL, NULL); 
		worker_threads[i] = CreateThread(NULL, 0, &Application_InternetExplorer::InternetExplorerWorker, (LPVOID)i, 0, NULL);
	}
}

Application_InternetExplorer::~Application_InternetExplorer(void)
{
	for(unsigned int i = 0; i < MAX_WORKER_THREADS; i++)
	{
		CloseHandle(worker_has_data[i]);
		CloseHandle(worker_finished[i]);
		CloseHandle(worker_threads[i]);
	}
}


void
Application_InternetExplorer::visitGroup(VisitEvent* visitEvent)
{	
	unsigned int n_visited_urls = 0;
	unsigned int to_visit = 0;
	unsigned int n_visiting = 0;
	int n_urls = visitEvent->getUrls().size();

	// Start the COM interface
	CoInitializeEx(NULL,COINIT_MULTITHREADED);

	IClassFactory* internet_explorer_factory;

	// Get a link to the IE factory for creating IE instances
	HRESULT hr = CoGetClassObject(CLSID_InternetExplorer,
		CLSCTX_LOCAL_SERVER,
		NULL,
		IID_IClassFactory,
		(void**)&internet_explorer_factory); 

	// Allocate on the heap so threads can use the information
	InternetExplorerInstance** iexplore_instances = (InternetExplorerInstance**)malloc(sizeof(InternetExplorerInstance*)*n_urls);
	for(unsigned int i = 0; i < n_urls; i++)
	{
		iexplore_instances[i] = new InternetExplorerInstance(internet_explorer_factory);
	}

	VISIT_INFO* visit_information = new VISIT_INFO[n_urls];

	// Loop until all urls have been visited
	while(n_visited_urls < n_urls)
	{
		for(unsigned int i = 0; i < MAX_WORKER_THREADS; i++)
		{
			if(!worker_thread_busy[i] && n_visiting < n_urls)
			{
				// Give the threads something to do					
				visit_information[to_visit].internet_explorer_instance = iexplore_instances[to_visit];
				visit_information[to_visit].url = visitEvent->getUrls().at(to_visit);
				worker_thread_data[i] = (LPVOID*)&visit_information[to_visit++];
				worker_thread_busy[i] = true;
				n_visiting++;
				SetEvent(worker_has_data[i]);	
			}
		}

		// Wait for one of the workers threads to finish
		DWORD dwWait = WaitForMultipleObjects(MAX_WORKER_THREADS, worker_finished, false, 60*1000);

		// If one has finished then a url has been visited
		int index = dwWait - WAIT_OBJECT_0;
		if(index < MAX_WORKER_THREADS && !worker_thread_busy[index])
		{
			n_visited_urls++;
		}
	}

	// Give the visit event a success or error code based on the visitaion of each url
	for(unsigned int i = 0; i < n_urls; i++)
	{
		Url* url = visitEvent->getUrls().at(i);
		visitEvent->setErrorCode(url->getMajorErrorCode());
	}

	// Create another fake IE instance so that we can close the process
	IWebBrowser2* pInternetExplorer;
	hr = internet_explorer_factory->CreateInstance(NULL, IID_IWebBrowser2, 
							(void**)&pInternetExplorer);
	HWND hwndIE;
	DWORD dProcessID;
	pInternetExplorer->get_HWND((SHANDLE_PTR*)&hwndIE);
	GetWindowThreadProcessId(hwndIE, &dProcessID);

	// Close the IE process
	HANDLE hProc = OpenProcess(PROCESS_TERMINATE, FALSE, dProcessID);
	if(hProc != NULL)
	{
		if(!TerminateProcess(hProc, 0))
		{
			visitEvent->setErrorCode(CAPTURE_VISITATION_PROCESS_ERROR);
		}
	} else {
		visitEvent->setErrorCode(CAPTURE_VISITATION_PROCESS_ERROR);
	}
	pInternetExplorer->Release();

	//Delete all IE instance objects
	delete [] visit_information;
	for(unsigned int i = 0; i < n_urls; i++)
	{
		delete iexplore_instances[i];
	}
	free(iexplore_instances);

	// Free the COM interface stuff
	internet_explorer_factory->Release();
	CoUninitialize();
}

wchar_t**
Application_InternetExplorer::getSupportedApplicationNames()
{
	return supportedApplications;
}