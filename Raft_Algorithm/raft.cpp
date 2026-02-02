#define _WIN32_WINNT 
#pragma comment(lib, "ws2_32.lib")
#include<iostream>
#include<fstream>
#include<vector>
#include<set>
#include<string>
#include<map>
#include "mingw-std-threads/mingw.mutex.h"
#include<chrono>
#include"mingw-std-threads/mingw.thread.h"
#include<winsock2.h>
#include<ws2tcpip.h>
#include<sstream>



using namespace std;
vector<string> split(const string& s,char delimiter){
    vector<string> tokens;
    string token;
    istringstream tokenStream(s);
    while(getline(tokenStream,token,delimiter)){
        tokens.push_back(token);
    }
    return tokens;
}
struct Transaction{
    int id;
    string from;
    string to;
    double amount;
};

struct LogEntry{
    int term;
    Transaction tx;
};

enum Role{Follower,Leader,Candidate};

class stable_storage{
    string filePath;
    public:
    stable_storage(int nodeID){
        filePath="node_"+ to_string(nodeID)+"_storage.txt";
    }
    void save(int term,int votedFor,int commitLength,const vector<LogEntry> &log){
        ofstream out(filePath,ios::trunc);
        out << term << " " << votedFor << " " << commitLength << endl;
        out<<log.size()<<endl;
        for(const auto &entry : log){
            out<<entry.term<< " " << entry.tx.amount<<" "<<entry.tx.from<< " "<<entry.tx.to<<" "<<entry.tx.id<<endl;
        }
        
        out.close();
    }
    bool load(int& term,int& votedFor,int& commitLength,vector<LogEntry>&log){
        ifstream in(filePath);
        if(!in.is_open()) return false;
        int logSize;
        if (!(in >> term >> votedFor >> commitLength >> logSize)) return false;

        log.clear();
        for (int i = 0; i < logSize; ++i) {
            LogEntry entry;
            in >> entry.term >> entry.tx.amount >> entry.tx.from >> entry.tx.to >> entry.tx.id;
            log.push_back(entry);
        }
        return true;
    }
};

class RaftNode;

class NetworkManager{
    int port;
    SOCKET server_fd;
    map<int,int> peers;

    public:
    NetworkManager(int p,map<int,int> clusterPeers):port(p),peers(clusterPeers){
        WSADATA wsadata;
        if(WSAStartup(MAKEWORD(2,2),&wsadata)!=0){
            cerr<<"Winsock Initialization failed"<<endl;
        }
    }
    ~NetworkManager(){
        closesocket(server_fd);
        WSACleanup();
    }

    void sendToPeer(int peerID,string message){
        if(peers.find(peerID)==peers.end()){
            return;
        }
        SOCKET sock = socket(AF_INET,SOCK_STREAM,0);
        if(sock==INVALID_SOCKET) return;

        sockaddr_in serv_addr;
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_port = htons(peers[peerID]);
        serv_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

        DWORD timeout=100;
        setsockopt(sock,SOL_SOCKET,SO_SNDTIMEO,(char*)&timeout, sizeof(timeout));
        cout << "[NET] Trying to connect to Node " << peerID << " on port " << peers[peerID] << endl;
        if (connect(sock, (struct sockaddr*)&serv_addr,sizeof(serv_addr)) != SOCKET_ERROR){
            send(sock,message.c_str(),(int)message.length(),0);
        }
        cout << "[NET] Sent message to Node " << peerID << ": " << message << endl;

        closesocket(sock);
    }

    void broadcast(string message,int selfID){
        for(auto const& x : peers){
            int peerID = x.first;
            int peerPort = x.second;
            if(peerID==selfID){
                continue;
            }
            thread([this,peerID,message](){
                this->sendToPeer(peerID,message);
            }).detach();
        }
    }
    void startServer(RaftNode* node);
};

class RaftNode{
    private:
    int currentTerm;
    int votedFor;
    vector<LogEntry> log;
    int commitLength;

    Role currentRole;
    int currentLeader;
    set<int> votesRecieved;

    stable_storage storage;
    int nodeID;
    int last_applied;
    map<string,double> accounts;

    mutex raftMutex;
    chrono::steady_clock::time_point lastContact;
    int electionTimeout;
    NetworkManager nm;
    int clusterSize;

    public:

    RaftNode(int id,map<int,int> &peerPorts): nodeID(id),storage(id),last_applied(0),nm(peerPorts[id],peerPorts),clusterSize(peerPorts.size()) {
        currentRole = Follower;
        currentLeader=-1;
        electionTimeout=150+(rand()%150);

        if(!storage.load(currentTerm,votedFor,commitLength,log)){
            currentTerm=0;
            votedFor=-1;
            commitLength=0;
            persist(); 
            cout << "Created new storage file for Node " << nodeID << endl;
        }
        accounts["Alice"] = 1000.0;
        accounts["Bob"] = 1000.0;
        thread(&NetworkManager::startServer, &nm, this).detach();
        thread(&RaftNode::runTimer,this).detach();
    }
    void persist(){
        storage.save(currentTerm,votedFor,commitLength,log);
    }
    void runTimer(){
        while(true){
            this_thread::sleep_for(chrono::milliseconds(10));
            lock_guard<mutex> lock(raftMutex);
            auto now=chrono::steady_clock::now();
            auto duration =chrono::duration_cast<chrono::milliseconds>(now-lastContact).count();

            if(currentRole != Leader && duration >electionTimeout){
                startElection();
            }
            if(currentRole==Leader){
                sendHeartbeats();
            }
        }
    }
    void handleAppendEntries(int term, int leaderID, int prevLogIdx, int prevLogTerm, int leaderCommit, vector<string>& parts) {
        if (term < currentTerm) {
            string reject = "APPEND_ACK|" + to_string(nodeID) + "|" + to_string(currentTerm) + "|0|0";
            nm.sendToPeer(leaderID, reject);
            return;
        }

        if (term > currentTerm) {
            currentTerm = term;
            votedFor = -1;
        }
        currentRole = Follower;
        currentLeader = leaderID;
        lastContact = chrono::steady_clock::now();
        persist(); 

    
        bool logOk = false;
        if (prevLogIdx == -1) {
            logOk = true; 
        } else if (prevLogIdx < log.size() && log[prevLogIdx].term == prevLogTerm) {
            logOk = true; 
        }

        if (!logOk) {
            string reject = "APPEND_ACK|" + to_string(nodeID) + "|" + to_string(currentTerm) + "|0|0";
            nm.sendToPeer(leaderID, reject);
            return;
        }

        int partsIdx = 6;
        int currentLogIdx = prevLogIdx + 1;

        while (partsIdx < parts.size()) {
            int eTerm = stoi(parts[partsIdx++]);
            int eId = stoi(parts[partsIdx++]);
            string eFrom = parts[partsIdx++];
            string eTo = parts[partsIdx++];
            double eAmt = stod(parts[partsIdx++]);

            Transaction tx = {eId, eFrom, eTo, eAmt};
            LogEntry newEntry = {eTerm, tx};

            if (currentLogIdx < log.size()) {
                if (log[currentLogIdx].term != eTerm) {
                    log.resize(currentLogIdx); 
                    log.push_back(newEntry);
                }
            } else {
                log.push_back(newEntry);
            }
            currentLogIdx++;
        }
    
        if (parts.size() > 6) persist(); 

        if (leaderCommit > commitLength) {
            commitLength = min(leaderCommit, (int)log.size() - 1); 
        
            cout << "Node " << nodeID << " committed up to index " << commitLength << endl;
        }

        int myMatchIdx = (int)log.size() - 1;
        string accept = "APPEND_ACK|" + to_string(nodeID) + "|" + to_string(currentTerm) + "|1|" + to_string(myMatchIdx);
        nm.sendToPeer(leaderID, accept);
    }
    void sendHeartbeats() {
        string msg = "HEARTBEAT|" + to_string(nodeID) + "|" + 
                     to_string(currentTerm) + "|" + to_string(commitLength);
        
        nm.broadcast(msg, nodeID);
    }


    void startElection() {
        currentRole = Candidate;
        currentTerm++;
        votedFor = nodeID;
        votesRecieved.clear();
        votesRecieved.insert(nodeID); 

        lastContact = chrono::steady_clock::now();
        persist();
        cout << "Node " << nodeID << " became Candidate for Term " << currentTerm << endl;
        int lastLogIndex = (int)log.size() - 1;
        int lastLogTerm = 0;
        if (lastLogIndex >= 0) {
            lastLogTerm = log[lastLogIndex].term;
        }
        string msg = "VOTE_REQ|" + to_string(nodeID) + "|" + 
                 to_string(currentTerm) + "|" + 
                 to_string(lastLogIndex) + "|" + 
                 to_string(lastLogTerm);
        nm.broadcast(msg, nodeID);
    }

    void onClientRequest(Transaction tx){
        lock_guard<mutex> lock(raftMutex);
        if(currentRole!=Leader){
            return;
        }
        log.push_back({currentTerm,tx});
        persist();
    }

    void applyToStateMachine(){
        while(last_applied < commitLength){
            Transaction tx=log[last_applied].tx;
            if(accounts[tx.from]>=tx.amount){
                accounts[tx.from]-=tx.amount;
                accounts[tx.to]+=tx.amount;
            }
            last_applied++;
        }
        
    }

    void onReceiveHeartbeat(int leaderID,int term,int leaderCommit){
        lock_guard<mutex> lock(raftMutex);
        if(term>=currentTerm){
            currentTerm=term;
            currentRole=Follower;
            currentLeader=leaderID;
            lastContact=chrono::steady_clock::now();

            if(leaderCommit >commitLength){
                commitLength=min(leaderCommit,(int)log.size());
                applyToStateMachine();
            }
        }
    }
    void handleIncomingMessage(string message) {
        lock_guard<mutex> lock(raftMutex);
        cout << "[RECV] Raw Message: " << message << endl;
        vector<string> parts = split(message, '|');
        if (parts.empty()) return;

        string type = parts[0];

        if (type == "HEARTBEAT") {
            int lID = stoi(parts[1]);
            int trm = stoi(parts[2]);
            int cLen = stoi(parts[3]);
            
        }
        
        else if (type == "VOTE_REQ") {
            int cID = stoi(parts[1]);
            int cTerm = stoi(parts[2]);
            int cLogIdx = stoi(parts[3]);
            int cLogTerm = stoi(parts[4]);
            if (cTerm < currentTerm) {
                return; 
            }

            if (cTerm > currentTerm) {
                currentTerm = cTerm;
                currentRole = Follower;
                votedFor = -1;
                persist();
            }

            int myLogIdx = (int)log.size() - 1;
            int myLogTerm = (myLogIdx >= 0) ? log[myLogIdx].term : 0;

            bool logIsOk = (cLogTerm > myLogTerm) || 
                   (cLogTerm == myLogTerm && cLogIdx >= myLogIdx);

            if (logIsOk && (votedFor == -1 || votedFor == cID)) {
                votedFor = cID;
                persist();
                lastContact = chrono::steady_clock::now(); 
        
                cout << "Voted FOR Node " << cID << " in Term " << currentTerm << endl;

                string voteMsg = "VOTE_ACK|" + to_string(nodeID) + "|" + 
                         to_string(currentTerm);
                nm.sendToPeer(cID, voteMsg);
            }
        }
        else if (type == "VOTE_ACK") {
    
            int voterID = stoi(parts[1]);
            int vTerm = stoi(parts[2]);
            if (currentRole == Candidate && vTerm == currentTerm) {
                votesRecieved.insert(voterID);
                cout << "Node " << nodeID << " received vote from " << voterID << endl;

                int clusterSize = 3;
        
                if (votesRecieved.size() >= clusterSize / 2) {
                    becomeLeader();
                }
            }
        }
    }

    void becomeLeader() {
        currentRole = Leader;
        currentLeader = nodeID;
        cout << "!!! Node " << nodeID << " won the election! Becoming LEADER for Term " << currentTerm << " !!!" << endl;
    
    
        sendHeartbeats();
    }
    
};

void NetworkManager::startServer(RaftNode* node) {
        server_fd = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address;
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = INADDR_ANY;
        address.sin_port = htons(port);

        if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) == SOCKET_ERROR) {
            cerr << "Bind failed for port " << port << endl;
            return;
        }
        listen(server_fd, 3);
        cout << "[SERVER] Ear is listening on Port " << port << endl;
        while (true) {
            SOCKET new_socket = accept(server_fd, NULL, NULL);
            if (new_socket != INVALID_SOCKET) {
                thread([new_socket, node]() {
                    char buffer[1024] = {0};
                    int valread = recv(new_socket, buffer, 1024, 0);
                    if (valread > 0) {
                        node->handleIncomingMessage(string(buffer));
                    }
                    closesocket(new_socket);
                }).detach();
            }
        }
    }





int main() {
    map<int, int> peerPorts;
    peerPorts[0] = 8001; 
    peerPorts[1] = 8002; 
    peerPorts[2] = 8003; 

    int myID;
    cout << "Enter Node ID (0, 1, or 2): ";
    cin >> myID;

    if (peerPorts.find(myID) == peerPorts.end()) {
        cerr << "Invalid ID! Must be 0, 1, or 2." << endl;
        return -1;
    }

    cout << "Starting Node " << myID << " on Port " << peerPorts[myID] << "..." << endl;

    
    RaftNode node0(0, peerPorts);
    RaftNode node1(1, peerPorts);
    RaftNode node2(2, peerPorts);
    
    while (true) {
        
        this_thread::sleep_for(chrono::seconds(1));
    }

    return 0;
}