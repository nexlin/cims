## 빌드/배포 플로우

  # 1. C++ 빌드 + dist 생성 + Web UI 빌드                                         
  ./cims.sh build                                                                 
                                                                                  
  # 2. IP/도메인 설정 (단일 서버)                                                 
  ./configure.sh --local-ip 192.168.1.10 --db-password mypass --sip-domain      
  ims.company.com                                                                 
                                                                                
  # 3. 시작    
  ```bash
  ./cims.sh start                                                               
  ```

  # 다중 서버 배포 시:                                                              
  ```bash
  ./configure.sh --csp-ip 192.168.1.10 --cmp-ip 192.168.1.11 \
                 --cwrtc-ip 192.168.1.12 --csc-host 192.168.1.13 \                
                 --db-host 192.168.1.14                                           
  ```
                                                                                  
  # dist/ 구조                                                                      

  build/dist/                                                                   
    cmp/bin/  cmp/config/cmp.json   ← @PLACEHOLDER@ → configure.sh로 채움         
    csp/bin/  csp/config/csp.json   user/ group/ route/ cert/                     
    cwrtc/bin/ cwrtc/config/cwrtc.json  html/ cert/                               
    csc/src/  csc/config/csc.json                                                 
    console/  (npm build 후 dist/ 서빙)                                           
    phone/    (npm build 후 dist/ 서빙)                                           
    cspsim/bin/                                                                   
    configure.sh  cims.sh   ← 배포 서버에서도 동일하게 사용                       
                                                                                  
  # 배포 서버에서: dist/ 전체를 복사 후 ./configure.sh --local-ip <실제IP> →        
  ```bash
  ./cims.sh start                             
  ```