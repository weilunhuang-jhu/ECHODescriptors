/*
Copyright (c) 2020, Michael Kazhdan and Thomas Mitchel
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

Redistributions of source code must retain the above copyright notice, this list of
conditions and the following disclaimer. Redistributions in binary form must reproduce
the above copyright notice, this list of conditions and the following disclaimer
in the documentation and/or other materials provided with the distribution. 

Neither the name of the Johns Hopkins University nor the names of its contributors
may be used to endorse or promote products derived from this software without specific
prior written permission. 

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY
EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO THE IMPLIED WARRANTIES 
OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT
SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
TO, PROCUREMENT OF SUBSTITUTE  GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
DAMAGE.
*/

#include <stdio.h>
#include <stdlib.h>
#include <fstream>

#include <Misha/Ply.h>
#include <Misha/CmdLineParser.h>
#include <Misha/Geometry.h>
#include <Misha/Image.h>
#include <Misha/TriMesh.h>
#include <Misha/Miscellany.h>
#include <Misha/PlyVertexData.h>
#include <Misha/Miscellany.h>
#include "GetDescriptor.inl"


Misha::CmdLineParameter< std::string > In( "in" ) , Spec( "spec" ) , Out( "out" ); 
Misha::CmdLineParameter< int > SourceNode( "vertex" ) , RadialBins( "rBins" , 5 ) , OutResolution( "resolution" ) , SourceFace( "tri") , DistanceType( "distance" , DISTANCE_BIHARMONIC );
Misha::CmdLineParameter< float > threshFactor( "tau" , 0.08f ) , Deviation( "dev" , -1. );
Misha::CmdLineParameterArray< float , 3 > BC( "bc" );
Misha::CmdLineReadable  Verbose( "verbose" ) , DiskSupport( "disk" );

Misha::CmdLineReadable* params[] =
{
    &In ,
    &Spec ,
    &Out ,
    &SourceNode ,
    &RadialBins ,
    &threshFactor ,
    &Verbose ,
    &Deviation ,
    &OutResolution ,
    &DiskSupport ,
    &SourceFace ,
    &BC ,
    &DistanceType ,
    NULL
};

void ShowUsage( const char* ex )
{
    printf( "Usage %s:\n" , ex );
    printf( "\t --%s <input mesh>\n" , In.name.c_str() );
    printf( "\t --%s <source vertex index>\n" , SourceNode.name.c_str() );
    printf( "\t --%s <source face index>\n" , SourceFace.name.c_str() );
    printf( "\t --%s <barycentric coordinates 1> <barycentric coordinates 2> <barycentric coordinates 2>\n" , BC.name.c_str() );
    printf( "\t[--%s <spectral decomposition>]\n" , Spec.name.c_str() );
    printf( "\t[--%s <output ECHO descriptor>]\n" , Out.name.c_str() );
    printf( "\t[--%s <Mesh area to support radius scale>=%.2f]\n" , threshFactor.name.c_str() , threshFactor.value );
    printf( "\t[--%s <histogram radius (in bin units)>=%d, size of histogram will be (2 * n + 1)^2]\n" , RadialBins.name.c_str() , RadialBins.value );
    printf( "\t[--%s <target deviation (for color output)>=%f]\n" , Deviation.name.c_str() , Deviation.value );
    printf( "\t[--%s <resampled output resolution>=<histogram radius>*2+1]\n" , OutResolution.name.c_str() );
    printf( "\t[--%s <distance type>=%d]\n" , DistanceType.name.c_str() , DistanceType.value );
    for( int i=0 ; i<DISTANCE_COUNT ; i++ ) printf( "\t\t%d] %s\n" , i , DistanceNames[i].c_str() );
    printf( "\t[--%s]\n" , Verbose.name.c_str() );
    printf( "\t[--%s]\n" , DiskSupport.name.c_str() );
}

template< typename Real > using PlyVertexFactory = VertexFactory::Factory< Real , VertexFactory::PositionFactory< Real , 3 > >;
template< typename Real > using PlyVertex = typename PlyVertexFactory< Real >::VertexType;

RegularGrid< float , 2 > TransposeSignal( const RegularGrid< float , 2 > &in )
{
    RegularGrid< float , 2 > out;
    out.resize( in.res(1) , in.res(0) );
    for( unsigned int i=0 ; i<in.res(0) ; i++ ) for( unsigned int j=0 ; j<in.res(1) ; j++ ) out(j,i) = in(i,j);
    return out;
}

RegularGrid< float , 2 > ResampleSignal( const RegularGrid< float , 2 > &in , unsigned int resX , unsigned int resY )
{
    RegularGrid< float , 2 > out;
    out.resize( resX , resY );

    for( unsigned int i=0 ; i<resX ; i++ ) for( unsigned int j=0 ; j<resY ; j++ )
    {
        float x = (float)( ((double)i)/(resX-1) * (in.res(0)-1) );
        float y = (float)( ((double)j)/(resY-1) * (in.res(1)-1) );
        out(i,j) = in(x,y);
    }
    return out;
}

RegularGrid< float , 2 > ResampleSignalDisk( const RegularGrid< float , 2 > &in , unsigned int resX , unsigned int resY )
{
    RegularGrid< float , 2 > out;
    out.resize( resX , resY );

    int diskCenter = (in.res(0) - 1) / 2;

    for( unsigned int i=0 ; i<resX ; i++ ) for( unsigned int j=0 ; j<resY ; j++ )
    {
        float x = (float)( ((double)i)/(resX-1) * (in.res(0)-1) );
        float y = (float)( ((double)j)/(resY-1) * (in.res(1)-1) );
        if ( (x - diskCenter) * (x - diskCenter) + (y - diskCenter) * (y - diskCenter) <= diskCenter * diskCenter )
        {        
           out(i,j) = in(x,y);
        }
        else
        {
           out(i, j) = std::numeric_limits< float >::infinity();
        }

    }
    return out;
}
//-------------------------------------------------------------------------------------------------------------------------------------------------------------
// MAIN
//-------------------------------------------------------------------------------------------------------------------------------------------------------------
template< class Real >
void run( void )
{
    Miscellany::Timer timer;
    std::vector< double > signalValues;

    int nRadialBins = RadialBins.value;

    //==Load Mesh===
    std::vector< TriangleIndex > triangles;
    std::vector< Point3D< float > > vertices;
    timer.reset();
    {
        int fileType;
        PlyVertexFactory< float > vFactory;
        std::vector< PlyVertex< float > > _vertices;
        PLY::ReadTriangles( In.value , vFactory , _vertices , triangles , NULL , fileType );
        vertices.resize( _vertices.size() );
        for( int i=0 ; i<_vertices.size() ; i++ ) vertices[i] = Point3D< float >( _vertices[i].get<0>() );
    }

    //=== Load PLY=====
    TriMesh< float > tMesh( vertices , triangles );
    if( Verbose.set ) std::cout << "Got mesh: " << timer.elapsed() << std::endl;

    //==Load Spectral Decomposition==
    timer.reset();
    if( Spec.set ) tMesh.readSpectralDecomposition( Spec.value );
    else tMesh.setSpectralDecomposition();
    std::cout << "Got spectrum: " << timer.elapsed() << std::endl;

    // Compute + smooth HKS
    std::vector< double > hks;

    timer.reset();
    tMesh.vertexHKS( hks , 0.1 ); // 0.1 
    if( Verbose.set ) std::cout << "Got HKS: " << timer.elapsed() << std::endl;

    timer.reset();
    tMesh.smoothVertexSignal( hks , 1.0e7 ); 
    if( Verbose.set ) std::cout << "Smoothed HKS: " << timer.elapsed() << std::endl;

    std::vector< Point2D< double > > triangleGradients;
    timer.reset();
    tMesh.initMetricsBiharmonic();
    tMesh.metricGradient( hks , triangleGradients );
    if( Verbose.set ) std::cout << "Got HKS gradients: " << timer.elapsed() << std::endl;

    // Compute support radius proportional to surface area
    tMesh.initAreaBiharmonic();

    float rho = (float)( threshFactor.value * std::sqrt( tMesh.totalArea () / M_PI ) );

    if( SourceNode.set && SourceNode.value<0 )
    {
        timer.reset();
        for( int i=0 ; i<-SourceNode.value ; i++ ) RegularGrid< float , 2 > F = echo< float >( tMesh , triangleGradients, rand() % vertices.size() , rho , nRadialBins , DistanceType.value );
        if( Verbose.set ) std::cout << "Got " << (-SourceNode.value) << " ECHO descriptors: " << timer.elapsed() << std::endl;
    }
    else
    {
        // Compute ECHO descriptor
        RegularGrid< float , 2 > F;

        timer.reset();
        if( SourceNode.set ) F = echo< float >( tMesh , triangleGradients, SourceNode.value , rho , nRadialBins , DistanceType.value );
        else                 F = echo< float >( tMesh , triangleGradients , std::pair< int , Point3D< double > >( SourceFace.value , Point3D< double >( BC.values[0] , BC.values[1] , BC.values[2] ) ) , rho , nRadialBins , DistanceType.value );
        if( Verbose.set ) std::cout << "Got ECHO descriptor: " << timer.elapsed() << std::endl;

        if( DiskSupport.set ) F = ResampleSignalDisk( F , OutResolution.value , OutResolution.value );
        else                  F = ResampleSignal( F , OutResolution.value , OutResolution.value );

        F = TransposeSignal( F );

        if( Out.set )
        {
            std::string ext = Misha::ToLower( Misha::GetFileExtension( Out.value ) );
            if( ext==std::string( "txt" ) )
            {
                std::ofstream echoD;
                echoD.open( Out.value );

                for( unsigned int x=0 ; x<F.res(0) ; x++ ) for( unsigned int y=0 ; y<F.res(1) ; y++ )
                {
                    if( x==F.res(0)-1 && y==F.res(1)-1 ) echoD << F(x,y) << std::endl;
                    else                                 echoD << F(x,y) << " ";
                }

                echoD.close();
            }
            else
            {
                unsigned char *pixels = new unsigned char[ F.resolution()*3 ];

                int count = 0;
                double dev = 0;
                double sumD = 0;
                for( int i=0 ; i<F.resolution() ; i++ ) if( F[i]<std::numeric_limits< float >::infinity() ) dev += F[i] * F[i] , sumD += F[i], count++;

                dev = sqrt( dev/count );
                double hue , saturation;
                if( Deviation.value<=0 )
                {
                    saturation = 0.;
                    hue = 0;
                    std::cout << "Deviation: " << dev << std::endl;
                    std::cout << "Sum: " << sumD << std::endl;
                }
                else
                {
                    saturation = 1.;
                    hue = 4.*M_PI/3. * dev / Deviation.value;
                }
                for( int i=0 ; i<F.resolution() ; i++ )
                {
                    if( F[i]==std::numeric_limits< float >::infinity() ) pixels[3*i+0] = pixels[3*i+1] = pixels[3*i+2] = 255;
                    else
                    {
                        double d = std::max( 0. , std::min( F[i] / (3.*dev) , 1. ) );
                        Point3D< double > rgb , hsv( hue , saturation , d );
                        Miscellany::HSVtoRGB( &hsv[0] , &rgb[0] );
                        for( int c=0 ; c<3 ; c++ ) pixels[3*i+c] = (unsigned char)(int)floor( rgb[c] * 255 );
                    }
                }
                ImageWriter::Write( Out.value.c_str() , pixels , OutResolution.value , OutResolution.value , 3 );
                delete[] pixels;
            }
        }
    }
}

int main( int argc , char* argv[] )
{
    Misha::CmdLineParse( argc-1 , argv+1 , params );

    if( !In.set || ( !SourceNode.set && !SourceFace.set ) || ( SourceFace.set && !BC.set ) )
    {
        ShowUsage( argv[0] );
        return EXIT_FAILURE;
    }
    if( !OutResolution.set ) OutResolution.value = RadialBins.value * 2 + 1;

    Miscellany::Timer timer;
    run< float >();
    if( Verbose.set ) std::cout << "Got desriptor(s) in: " << timer.elapsed() << std::endl;

    return 1;
}
